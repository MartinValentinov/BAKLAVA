using Dapper;
using DarkVessel.Core;
using MySqlConnector;

namespace DarkVessel.Infrastructure;

public sealed class AisStore : IAisSource
{
    private readonly string _connectionString;

    public AisStore(string connectionString) => _connectionString = connectionString;

    private async Task<MySqlConnection> OpenAsync(CancellationToken ct)
    {
        var conn = new MySqlConnection(_connectionString);
        await conn.OpenAsync(ct).ConfigureAwait(false);
        return conn;
    }

    public async Task<IReadOnlyList<AisPosition>> CandidatesNearAsync(
        double lat, double lon, DateTime when, double windowMinutes, double radiusKm,
        CancellationToken ct = default)
    {
        var half = TimeSpan.FromMinutes(windowMinutes);
        var lo = when - half;
        var hi = when + half;
        var (latLo, latHi, lonLo, lonHi) = GeoUtils.BoundingBox(lat, lon, radiusKm);

        const string sql = """
            SELECT mmsi, ts, lat, lon, sog, cog, heading FROM ais_positions
            WHERE ts BETWEEN @lo AND @hi
              AND lat BETWEEN @latLo AND @latHi
              AND lon BETWEEN @lonLo AND @lonHi
            """;

        await using var conn = await OpenAsync(ct).ConfigureAwait(false);
        var rows = await conn.QueryAsync<PositionRow>(
            new CommandDefinition(sql, new { lo, hi, latLo, latHi, lonLo, lonHi }, cancellationToken: ct)
        ).ConfigureAwait(false);

        return rows
            .Select(r => new AisPosition(
                r.mmsi,
                DateTime.SpecifyKind(r.ts, DateTimeKind.Utc),
                r.lat, r.lon, r.sog, r.cog, r.heading))
            .ToList();
    }

    public async Task<bool> HadCoverageAsync(DateTime when, double slackMinutes = 5.0, CancellationToken ct = default)
    {
        var slack = TimeSpan.FromMinutes(slackMinutes);
        var lo = when - slack;
        var hi = when + slack;

        const string sql = """
            SELECT COUNT(*) FROM ais_coverage
            WHERE started_at <= @hi
              AND COALESCE(ended_at, last_message_at, started_at) >= @lo
            """;

        await using var conn = await OpenAsync(ct).ConfigureAwait(false);
        long count = await conn.ExecuteScalarAsync<long>(
            new CommandDefinition(sql, new { lo, hi }, cancellationToken: ct)
        ).ConfigureAwait(false);
        return count > 0;
    }

    private sealed record PositionRow(long mmsi, DateTime ts, double lat, double lon, double? sog, double? cog, double? heading);

    public async Task<long> OpenCoverageAsync(string bboxesJson, string host, CancellationToken ct = default)
    {
        const string sql = """
            INSERT INTO ais_coverage (started_at, last_message_at, ended_at, bboxes, messages, host)
            VALUES (UTC_TIMESTAMP(), NULL, NULL, @bboxesJson, 0, @host);
            SELECT LAST_INSERT_ID();
            """;
        await using var conn = await OpenAsync(ct).ConfigureAwait(false);
        return await conn.ExecuteScalarAsync<long>(
            new CommandDefinition(sql, new { bboxesJson, host }, cancellationToken: ct)
        ).ConfigureAwait(false);
    }

    public async Task TouchCoverageAsync(long id, int messages, DateTime? lastMessageAt, CancellationToken ct = default)
    {
        const string sql = """
            UPDATE ais_coverage
               SET last_message_at = COALESCE(@lastMessageAt, last_message_at), messages = @messages
             WHERE id = @id
            """;
        await using var conn = await OpenAsync(ct).ConfigureAwait(false);
        await conn.ExecuteAsync(
            new CommandDefinition(sql, new { id, messages, lastMessageAt }, cancellationToken: ct)
        ).ConfigureAwait(false);
    }

    public async Task CloseCoverageAsync(long id, int messages, CancellationToken ct = default)
    {
        const string sql = """
            UPDATE ais_coverage
               SET ended_at = UTC_TIMESTAMP(),
                   last_message_at = COALESCE(last_message_at, UTC_TIMESTAMP()),
                   messages = GREATEST(messages, @messages)
             WHERE id = @id AND ended_at IS NULL
            """;
        await using var conn = await OpenAsync(ct).ConfigureAwait(false);
        await conn.ExecuteAsync(
            new CommandDefinition(sql, new { id, messages }, cancellationToken: ct)
        ).ConfigureAwait(false);
    }

    public async Task<int> InsertPositionsAsync(IReadOnlyList<PositionWrite> rows, CancellationToken ct = default)
    {
        if (rows.Count == 0)
        {
            return 0;
        }
        const string sql = """
            INSERT IGNORE INTO ais_positions (mmsi, ts, lat, lon, sog, cog, heading, nav_status)
            VALUES (@Mmsi, @Ts, @Lat, @Lon, @Sog, @Cog, @Heading, @NavStatus)
            """;
        await using var conn = await OpenAsync(ct).ConfigureAwait(false);
        int written = 0;
        foreach (var chunk in rows.Chunk(200))
        {
            written += await conn.ExecuteAsync(new CommandDefinition(sql, chunk, cancellationToken: ct)).ConfigureAwait(false);
        }
        return written;
    }

    public async Task UpsertVesselsAsync(IReadOnlyList<VesselWrite> rows, CancellationToken ct = default)
    {
        if (rows.Count == 0)
        {
            return;
        }
        const string sql = """
            INSERT INTO vessels (mmsi, name, callsign, imo, ship_type, length_m, width_m, first_seen, last_seen)
            VALUES (@Mmsi, @Name, @Callsign, @Imo, @ShipType, @LengthM, @WidthM, @SeenAt, @SeenAt)
            ON DUPLICATE KEY UPDATE
                name       = COALESCE(VALUES(name), name),
                callsign   = COALESCE(VALUES(callsign), callsign),
                imo        = COALESCE(VALUES(imo), imo),
                ship_type  = COALESCE(VALUES(ship_type), ship_type),
                length_m   = COALESCE(VALUES(length_m), length_m),
                width_m    = COALESCE(VALUES(width_m), width_m),
                first_seen = LEAST(COALESCE(first_seen, VALUES(first_seen)), VALUES(first_seen)),
                last_seen  = GREATEST(COALESCE(last_seen, VALUES(last_seen)), VALUES(last_seen))
            """;
        await using var conn = await OpenAsync(ct).ConfigureAwait(false);
        foreach (var chunk in rows.Chunk(200))
        {
            await conn.ExecuteAsync(new CommandDefinition(sql, chunk, cancellationToken: ct)).ConfigureAwait(false);
        }
    }

    public async Task<ArchiveStats> ArchiveStatsAsync(CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct).ConfigureAwait(false);
        var sizeRow = await conn.QuerySingleOrDefaultAsync<(long? table_rows, long? bytes)>(
            new CommandDefinition("""
                SELECT table_rows, data_length + index_length AS bytes
                  FROM information_schema.TABLES
                 WHERE table_schema = DATABASE() AND table_name = 'ais_positions'
                """, cancellationToken: ct)
        ).ConfigureAwait(false);
        var spanRow = await conn.QuerySingleOrDefaultAsync<(DateTime? oldest, DateTime? newest)>(
            new CommandDefinition("SELECT MIN(ts) AS oldest, MAX(ts) AS newest FROM ais_positions", cancellationToken: ct)
        ).ConfigureAwait(false);

        return new ArchiveStats(sizeRow.table_rows, sizeRow.bytes, spanRow.oldest, spanRow.newest);
    }
}
