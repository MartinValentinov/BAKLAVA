using System.Globalization;
using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Serialization;
using DarkVessel.Core;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;

namespace DarkVessel.Infrastructure;

public sealed class HttpAisSource : IAisArchive
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    private readonly HttpClient _http;
    private readonly ILogger<HttpAisSource> _logger;
    private readonly TimeZoneInfo _queryTimeZone;

    public HttpAisSource(HttpClient http, IOptions<BaklavaApiOptions> options, ILogger<HttpAisSource> logger)
    {
        _http = http;
        _logger = logger;
        var apiKey = options.Value.ApiKey;
        if (!string.IsNullOrEmpty(apiKey))
        {
            _http.DefaultRequestHeaders.Remove("X-API-Key");
            _http.DefaultRequestHeaders.Add("X-API-Key", apiKey);
        }
        _queryTimeZone = ResolveTimeZone(options.Value.QueryTimeZone);
    }

    private TimeZoneInfo ResolveTimeZone(string id)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            return TimeZoneInfo.Utc;
        }
        try
        {
            return TimeZoneInfo.FindSystemTimeZoneById(id);
        }
        catch (Exception ex) when (ex is TimeZoneNotFoundException or InvalidTimeZoneException)
        {
            _logger.LogError(ex,
                "Baklava:QueryTimeZone '{Id}' is not a timezone this machine knows; falling back to UTC. "
                + "Time-windowed AIS queries will return nothing until this is a valid id (e.g. Europe/Sofia).",
                id);
            return TimeZoneInfo.Utc;
        }
    }

    public async Task<IReadOnlyList<AisPosition>> CandidatesNearAsync(
        double lat, double lon, DateTime when, double windowMinutes, double radiusKm,
        CancellationToken ct = default)
    {
        var half = TimeSpan.FromMinutes(windowMinutes);
        var (latLo, latHi, lonLo, lonHi) = GeoUtils.BoundingBox(lat, lon, radiusKm);
        var bbox = FormattableString.Invariant($"{latLo},{lonLo},{latHi},{lonHi}");

        var url = "?resource=ais_positions"
            + "&since=" + Uri.EscapeDataString(FormatQueryTime(when - half))
            + "&until=" + Uri.EscapeDataString(FormatQueryTime(when + half))
            + "&bbox=" + Uri.EscapeDataString(bbox)
            + "&limit=5000";

        var envelope = await GetAsync<AisPositionsEnvelope>(url, ct).ConfigureAwait(false);
        if (envelope?.Data is null)
        {
            return Array.Empty<AisPosition>();
        }

        var positions = new List<AisPosition>(envelope.Data.Count);
        foreach (var row in envelope.Data)
        {
            if (!TryParseUtc(row.Ts, out var ts))
            {
                continue;
            }
            if (GeoUtils.HaversineKm(lat, lon, row.Lat, row.Lon) > radiusKm)
            {
                continue;
            }
            positions.Add(new AisPosition(row.Mmsi, ts, row.Lat, row.Lon, row.Sog, row.Cog, row.Heading));
        }
        return positions;
    }

    public async Task<bool> HadCoverageAsync(DateTime when, double slackMinutes = 5.0, CancellationToken ct = default)
    {
        var slack = TimeSpan.FromMinutes(slackMinutes);
        var url = "?resource=ais_positions"
            + "&since=" + Uri.EscapeDataString(FormatQueryTime(when - slack))
            + "&until=" + Uri.EscapeDataString(FormatQueryTime(when + slack))
            + "&limit=1";

        var envelope = await GetAsync<AisPositionsEnvelope>(url, ct).ConfigureAwait(false);
        return envelope?.Data is { Count: > 0 };
    }

    public Task<long> OpenCoverageAsync(string bboxesJson, string host, CancellationToken ct = default)
    {
        _logger.LogWarning(
            "No ais_coverage endpoint on the Baklava API -- coverage is being inferred from " +
            "ais_positions data (see HadCoverageAsync) instead of tracked explicitly. " +
            "This session's coverage id is synthetic, not a real database row.");
        return Task.FromResult(-DateTime.UtcNow.Ticks);
    }

    public Task TouchCoverageAsync(long id, int messages, DateTime? lastMessageAt, CancellationToken ct = default)
        => Task.CompletedTask;

    public Task CloseCoverageAsync(long id, int messages, CancellationToken ct = default)
        => Task.CompletedTask;

    public async Task<int> InsertPositionsAsync(IReadOnlyList<PositionWrite> rows, CancellationToken ct = default)
    {
        if (rows.Count == 0)
        {
            return 0;
        }
        int written = 0;
        foreach (var chunk in rows.Chunk(500))
        {
            var items = chunk.Select(r => new Dictionary<string, object?>
            {
                ["mmsi"] = r.Mmsi,
                ["ts"] = FormatUtc(r.Ts),
                ["lat"] = r.Lat,
                ["lon"] = r.Lon,
                ["sog"] = r.Sog,
                ["cog"] = r.Cog,
                ["heading"] = r.Heading,
                ["nav_status"] = r.NavStatus,
            }).ToList();

            var result = await PostAsync("?resource=ais_positions", items, ct).ConfigureAwait(false);
            written += result?.Inserted ?? 0;
        }
        return written;
    }

    public async Task UpsertVesselsAsync(IReadOnlyList<VesselWrite> rows, CancellationToken ct = default)
    {
        if (rows.Count == 0)
        {
            return;
        }
        foreach (var chunk in rows.Chunk(500))
        {
            var items = chunk.Select(r => new Dictionary<string, object?>
            {
                ["mmsi"] = r.Mmsi,
                ["name"] = r.Name,
                ["callsign"] = r.Callsign,
                ["imo"] = r.Imo,
                ["ship_type"] = r.ShipType,
                ["length_m"] = r.LengthM,
                ["width_m"] = r.WidthM,
                ["first_seen"] = FormatUtc(r.SeenAt),
                ["last_seen"] = FormatUtc(r.SeenAt),
            }).ToList();

            await PostAsync("?resource=vessels", items, ct).ConfigureAwait(false);
        }
    }

    public async Task<ArchiveStats> ArchiveStatsAsync(CancellationToken ct = default)
    {
        var envelope = await GetAsync<SummaryEnvelope>("?resource=summary", ct).ConfigureAwait(false);
        var archive = envelope?.Data?.Archive;
        if (archive is null)
        {
            return new ArchiveStats(null, null, null, null);
        }
        TryParseUtc(archive.Oldest, out var oldest);
        TryParseUtc(archive.Newest, out var newest);
        return new ArchiveStats(archive.Positions, null, oldest, newest);
    }

    private async Task<T?> GetAsync<T>(string relativeUrl, CancellationToken ct) where T : class
    {
        using var response = await _http.GetAsync(relativeUrl, ct).ConfigureAwait(false);
        var json = await response.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
        try
        {
            var result = JsonSerializer.Deserialize<T>(json, JsonOptions);
            return result;
        }
        catch (JsonException ex)
        {
            _logger.LogWarning(ex, "Baklava API returned unparseable JSON for {Url}: {Body}", relativeUrl, json);
            return null;
        }
    }

    private async Task<WriteResult?> PostAsync(string relativeUrl, object items, CancellationToken ct)
    {
        using var response = await _http.PostAsJsonAsync(relativeUrl, new { items }, JsonOptions, ct).ConfigureAwait(false);
        var json = await response.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
        var result = JsonSerializer.Deserialize<WriteResult>(json, JsonOptions);
        if (result is null || !result.Ok)
        {
            throw new InvalidOperationException($"Baklava API write to {relativeUrl} failed: {result?.Error ?? json}");
        }
        return result;
    }

    private static string FormatUtc(DateTime dt) => dt.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture);

    private string FormatQueryTime(DateTime utc) =>
        FormatUtc(TimeZoneInfo.ConvertTimeFromUtc(DateTime.SpecifyKind(utc, DateTimeKind.Utc), _queryTimeZone));

    private static bool TryParseUtc(string? s, out DateTime result)
    {
        if (s is not null && DateTime.TryParse(s, CultureInfo.InvariantCulture, DateTimeStyles.None, out var parsed))
        {
            result = DateTime.SpecifyKind(parsed, DateTimeKind.Utc);
            return true;
        }
        result = default;
        return false;
    }

    private sealed class AisPositionsEnvelope
    {
        public bool Ok { get; set; }
        public string? Error { get; set; }
        public List<AisPositionRow>? Data { get; set; }
    }

    private sealed class AisPositionRow
    {
        public long Mmsi { get; set; }
        public string Ts { get; set; } = "";
        public double Lat { get; set; }
        public double Lon { get; set; }
        public double? Sog { get; set; }
        public double? Cog { get; set; }
        public double? Heading { get; set; }
    }

    private sealed class SummaryEnvelope
    {
        public bool Ok { get; set; }
        public SummaryData? Data { get; set; }
    }

    private sealed class SummaryData
    {
        [JsonPropertyName("archive")]
        public ArchiveSummary? Archive { get; set; }
    }

    private sealed class ArchiveSummary
    {
        public long? Positions { get; set; }
        public string? Oldest { get; set; }
        public string? Newest { get; set; }
    }

    private sealed class WriteResult
    {
        public bool Ok { get; set; }
        public string? Error { get; set; }
        public int? Inserted { get; set; }
        public int? Written { get; set; }
    }
}