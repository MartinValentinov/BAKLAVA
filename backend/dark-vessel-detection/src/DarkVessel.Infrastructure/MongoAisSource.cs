using System.Globalization;
using DarkVessel.Core;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using MongoDB.Bson;
using MongoDB.Driver;

namespace DarkVessel.Infrastructure;

public sealed class MongoAisSource : IAisArchive
{
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    private static readonly string[] TsFormats = ["dd/MM/yyyy HH:mm", "dd/MM/yyyy H:mm"];

    private const string CanonicalTsFormat = "dd/MM/yyyy HH:mm";
    private const string UnpaddedTsFormat = "dd/MM/yyyy H:mm";

    private const int MaxMinuteKeys = 2880;

    private readonly IMongoCollection<BsonDocument> _positions;
    private readonly IMongoCollection<BsonDocument> _vessels;
    private readonly IMongoCollection<BsonDocument> _coverage;
    private readonly MongoAisOptions _options;
    private readonly TimeZoneInfo _archiveTimeZone;
    private readonly ILogger<MongoAisSource> _logger;
    private readonly Task _indexesReady;

    public MongoAisSource(IOptions<MongoAisOptions> options, ILogger<MongoAisSource> logger)
    {
        _options = options.Value;
        _logger = logger;

        if (string.IsNullOrWhiteSpace(_options.ConnectionString))
        {
            throw new InvalidOperationException(
                "Mongo:ConnectionString is not configured. Set it with " +
                "`dotnet user-secrets set \"Mongo:ConnectionString\" \"mongodb+srv://...\"` " +
                "or the MONGODB_URI environment variable -- never in appsettings.json, which is committed to git.");
        }

        _archiveTimeZone = ResolveTimeZone(_options.TimeZone);

        var database = new MongoClient(_options.ConnectionString).GetDatabase(_options.Database);
        _positions = database.GetCollection<BsonDocument>(_options.PositionsCollection);
        _vessels = database.GetCollection<BsonDocument>(_options.VesselsCollection);
        _coverage = database.GetCollection<BsonDocument>(_options.CoverageCollection);

        _indexesReady = EnsureIndexesAsync();
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
                "Mongo:TimeZone '{Id}' is not a timezone this machine knows; falling back to UTC. "
                + "Time-windowed AIS queries will return nothing until this is a valid id (e.g. Europe/Sofia).",
                id);
            return TimeZoneInfo.Utc;
        }
    }

    private async Task EnsureIndexesAsync()
    {
        if (!_options.EnsureIndexes)
        {
            return;
        }
        try
        {
            var keys = Builders<BsonDocument>.IndexKeys.Ascending("ts").Ascending("lat").Ascending("lon");
            await _positions.Indexes
                .CreateOneAsync(new CreateIndexModel<BsonDocument>(keys, new CreateIndexOptions { Name = "ts_lat_lon" }))
                .ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex,
                "Could not create the {Collection} ts/lat/lon index; matching still works but scans the whole collection",
                _options.PositionsCollection);
        }
    }

    public async Task<IReadOnlyList<AisPosition>> CandidatesNearAsync(
        double lat, double lon, DateTime when, double windowMinutes, double radiusKm,
        CancellationToken ct = default)
    {
        var half = TimeSpan.FromMinutes(windowMinutes);
        var (latLo, latHi, lonLo, lonHi) = GeoUtils.BoundingBox(lat, lon, radiusKm);

        var builder = Builders<BsonDocument>.Filter;
        var filter = builder.And(
            TimeWindowFilter(when - half, when + half),
            builder.Gte("lat", latLo), builder.Lte("lat", latHi),
            builder.Gte("lon", lonLo), builder.Lte("lon", lonHi));

        await _indexesReady.ConfigureAwait(false);
        var docs = await _positions
            .Find(filter)
            .Limit(_options.CandidateLimit)
            .ToListAsync(ct)
            .ConfigureAwait(false);

        var positions = new List<AisPosition>(docs.Count);
        foreach (var doc in docs)
        {
            if (!TryParseTs(doc.GetValue("ts", BsonNull.Value), out var ts))
            {
                continue;
            }
            var rowLat = AsDouble(doc.GetValue("lat", BsonNull.Value));
            var rowLon = AsDouble(doc.GetValue("lon", BsonNull.Value));
            if (rowLat is null || rowLon is null)
            {
                continue;
            }
            if (GeoUtils.HaversineKm(lat, lon, rowLat.Value, rowLon.Value) > radiusKm)
            {
                continue;
            }
            positions.Add(new AisPosition(
                AsInt64(doc.GetValue("mmsi", BsonNull.Value)) ?? 0,
                ts,
                rowLat.Value,
                rowLon.Value,
                AsDouble(doc.GetValue("sog", BsonNull.Value)),
                AsDouble(doc.GetValue("cog", BsonNull.Value)),
                AsDouble(doc.GetValue("heading", BsonNull.Value))));
        }
        return positions;
    }

    public async Task<bool> HadCoverageAsync(DateTime when, double slackMinutes = 5.0, CancellationToken ct = default)
    {
        var slack = TimeSpan.FromMinutes(slackMinutes);

        await _indexesReady.ConfigureAwait(false);
        var doc = await _positions
            .Find(TimeWindowFilter(when - slack, when + slack))
            .Project(Builders<BsonDocument>.Projection.Include("_id"))
            .Limit(1)
            .FirstOrDefaultAsync(ct)
            .ConfigureAwait(false);

        return doc is not null;
    }

    private FilterDefinition<BsonDocument> TimeWindowFilter(DateTime loUtc, DateTime hiUtc)
        => Builders<BsonDocument>.Filter.In("ts", MinuteKeys(loUtc, hiUtc));

    private IReadOnlyList<string> MinuteKeys(DateTime loUtc, DateTime hiUtc)
    {
        var lo = ToArchiveTime(loUtc);
        var hi = ToArchiveTime(hiUtc);

        var keys = new List<string>();
        var minute = new DateTime(lo.Year, lo.Month, lo.Day, lo.Hour, lo.Minute, 0);
        while (minute <= hi && keys.Count < MaxMinuteKeys)
        {
            keys.Add(minute.ToString(CanonicalTsFormat, Inv));
            if (minute.Hour < 10)
            {
                keys.Add(minute.ToString(UnpaddedTsFormat, Inv));
            }
            minute = minute.AddMinutes(1);
        }
        return keys;
    }

    public async Task<long> OpenCoverageAsync(string bboxesJson, string host, CancellationToken ct = default)
    {
        var id = DateTime.UtcNow.Ticks;
        var doc = new BsonDocument
        {
            ["_id"] = id,
            ["started_at"] = DateTime.UtcNow,
            ["last_message_at"] = BsonNull.Value,
            ["ended_at"] = BsonNull.Value,
            ["bboxes"] = bboxesJson,
            ["messages"] = 0,
            ["host"] = host,
        };
        await _coverage.InsertOneAsync(doc, cancellationToken: ct).ConfigureAwait(false);
        return id;
    }

    public async Task TouchCoverageAsync(long id, int messages, DateTime? lastMessageAt, CancellationToken ct = default)
    {
        var update = Builders<BsonDocument>.Update.Set("messages", messages);
        if (lastMessageAt is not null)
        {
            update = update.Set("last_message_at", lastMessageAt.Value);
        }
        await _coverage
            .UpdateOneAsync(Builders<BsonDocument>.Filter.Eq("_id", id), update, cancellationToken: ct)
            .ConfigureAwait(false);
    }

    public async Task CloseCoverageAsync(long id, int messages, CancellationToken ct = default)
    {
        var now = DateTime.UtcNow;
        var filter = Builders<BsonDocument>.Filter.And(
            Builders<BsonDocument>.Filter.Eq("_id", id),
            Builders<BsonDocument>.Filter.Eq("ended_at", BsonNull.Value));
        var update = Builders<BsonDocument>.Update
            .Set("ended_at", now)
            .Max("messages", messages);

        await _coverage.UpdateOneAsync(filter, update, cancellationToken: ct).ConfigureAwait(false);
        await _coverage.UpdateOneAsync(
            Builders<BsonDocument>.Filter.And(
                Builders<BsonDocument>.Filter.Eq("_id", id),
                Builders<BsonDocument>.Filter.Eq("last_message_at", BsonNull.Value)),
            Builders<BsonDocument>.Update.Set("last_message_at", now),
            cancellationToken: ct).ConfigureAwait(false);
    }

    public async Task<int> InsertPositionsAsync(IReadOnlyList<PositionWrite> rows, CancellationToken ct = default)
    {
        if (rows.Count == 0)
        {
            return 0;
        }

        int written = 0;
        foreach (var chunk in rows.Chunk(500))
        {
            var models = new List<WriteModel<BsonDocument>>(chunk.Length);
            foreach (var row in chunk)
            {
                var ts = FormatTs(row.Ts);
                var filter = Builders<BsonDocument>.Filter.And(
                    Builders<BsonDocument>.Filter.Eq("mmsi", row.Mmsi),
                    Builders<BsonDocument>.Filter.Eq("ts", ts));
                var update = Builders<BsonDocument>.Update
                    .SetOnInsert("mmsi", row.Mmsi)
                    .SetOnInsert("ts", ts)
                    .SetOnInsert("lat", row.Lat)
                    .SetOnInsert("lon", row.Lon)
                    .SetOnInsert("sog", ToBson(row.Sog))
                    .SetOnInsert("cog", ToBson(row.Cog))
                    .SetOnInsert("heading", ToBson(row.Heading))
                    .SetOnInsert("nav_status", ToBson(row.NavStatus));
                models.Add(new UpdateOneModel<BsonDocument>(filter, update) { IsUpsert = true });
            }

            var result = await _positions
                .BulkWriteAsync(models, new BulkWriteOptions { IsOrdered = false }, ct)
                .ConfigureAwait(false);
            written += result.Upserts.Count;
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
            var models = new List<WriteModel<BsonDocument>>(chunk.Length);
            foreach (var row in chunk)
            {
                var update = Builders<BsonDocument>.Update
                    .SetOnInsert("mmsi", row.Mmsi)
                    .Min("first_seen", row.SeenAt)
                    .Max("last_seen", row.SeenAt);

                update = SetIfPresent(update, "name", row.Name);
                update = SetIfPresent(update, "callsign", row.Callsign);
                update = SetIfPresent(update, "imo", row.Imo);
                update = SetIfPresent(update, "ship_type", row.ShipType);
                update = SetIfPresent(update, "length_m", row.LengthM);
                update = SetIfPresent(update, "width_m", row.WidthM);

                models.Add(new UpdateOneModel<BsonDocument>(
                    Builders<BsonDocument>.Filter.Eq("mmsi", row.Mmsi), update)
                { IsUpsert = true });
            }

            await _vessels
                .BulkWriteAsync(models, new BulkWriteOptions { IsOrdered = false }, ct)
                .ConfigureAwait(false);
        }
    }

    public async Task<ArchiveStats> ArchiveStatsAsync(CancellationToken ct = default)
    {
        var rows = await _positions.EstimatedDocumentCountAsync(cancellationToken: ct).ConfigureAwait(false);

        long? bytes = null;
        try
        {
            var stats = await _positions
                .Aggregate<BsonDocument>(
                    new BsonDocument[] { new("$collStats", new BsonDocument("storageStats", new BsonDocument())) },
                    cancellationToken: ct)
                .FirstOrDefaultAsync(ct)
                .ConfigureAwait(false);
            var storage = stats?["storageStats"].AsBsonDocument;
            if (storage is not null)
            {
                bytes = AsInt64(storage.GetValue("storageSize", BsonNull.Value))
                        + (AsInt64(storage.GetValue("totalIndexSize", BsonNull.Value)) ?? 0);
            }
        }
        catch (MongoException ex)
        {
            _logger.LogDebug(ex, "$collStats is not available on this cluster; reporting archive size as unknown");
        }

        var sortableTs = new BsonDocument("$concat", new BsonArray
        {
            new BsonDocument("$substrBytes", new BsonArray { "$ts", 6, 4 }),
            new BsonDocument("$substrBytes", new BsonArray { "$ts", 3, 2 }),
            new BsonDocument("$substrBytes", new BsonArray { "$ts", 0, 2 }),
            " ",
            new BsonDocument("$cond", new BsonArray
            {
                new BsonDocument("$gte", new BsonArray { new BsonDocument("$strLenBytes", "$ts"), 16 }),
                new BsonDocument("$substrBytes", new BsonArray { "$ts", 11, 5 }),
                new BsonDocument("$concat", new BsonArray
                {
                    "0",
                    new BsonDocument("$substrBytes", new BsonArray { "$ts", 11, 4 }),
                }),
            }),
        });

        var span = await _positions
            .Aggregate<BsonDocument>(
                new BsonDocument[]
                {
                    new("$group", new BsonDocument
                    {
                        ["_id"] = BsonNull.Value,
                        ["oldest"] = new BsonDocument("$min", sortableTs),
                        ["newest"] = new BsonDocument("$max", sortableTs),
                    }),
                },
                new AggregateOptions { AllowDiskUse = true },
                ct)
            .FirstOrDefaultAsync(ct)
            .ConfigureAwait(false);

        return new ArchiveStats(
            rows,
            bytes,
            ParseSortableTs(span?.GetValue("oldest", BsonNull.Value)),
            ParseSortableTs(span?.GetValue("newest", BsonNull.Value)));
    }

    private DateTime? ParseSortableTs(BsonValue? value)
    {
        if (value is null || !value.IsString
            || !DateTime.TryParseExact(value.AsString, "yyyyMMdd HH:mm", Inv, DateTimeStyles.None, out var parsed))
        {
            return null;
        }
        return FromArchiveTime(parsed);
    }

    private static UpdateDefinition<BsonDocument> SetIfPresent(
        UpdateDefinition<BsonDocument> update, string field, object? value)
        => value is null || (value is string s && string.IsNullOrWhiteSpace(s))
            ? update
            : update.Set(field, BsonValue.Create(value));

    private static BsonValue ToBson(double? value) => value is null ? BsonNull.Value : new BsonDouble(value.Value);

    private static BsonValue ToBson(int? value) => value is null ? BsonNull.Value : new BsonInt32(value.Value);

    private string FormatTs(DateTime utc) => ToArchiveTime(utc).ToString(CanonicalTsFormat, Inv);

    private DateTime ToArchiveTime(DateTime utc)
        => TimeZoneInfo.ConvertTimeFromUtc(DateTime.SpecifyKind(utc, DateTimeKind.Utc), _archiveTimeZone);

    private DateTime FromArchiveTime(DateTime archiveLocal)
        => DateTime.SpecifyKind(
            TimeZoneInfo.ConvertTimeToUtc(DateTime.SpecifyKind(archiveLocal, DateTimeKind.Unspecified), _archiveTimeZone),
            DateTimeKind.Utc);

    private bool TryParseTs(BsonValue value, out DateTime utc)
    {
        if (value.IsBsonDateTime)
        {
            utc = DateTime.SpecifyKind(value.ToUniversalTime(), DateTimeKind.Utc);
            return true;
        }
        if (value.IsString
            && DateTime.TryParseExact(value.AsString.Trim(), TsFormats, Inv, DateTimeStyles.None, out var parsed))
        {
            utc = FromArchiveTime(parsed);
            return true;
        }
        utc = default;
        return false;
    }

    private static double? AsDouble(BsonValue value)
    {
        if (value.IsNumeric)
        {
            return value.ToDouble();
        }
        if (value.IsString && double.TryParse(value.AsString, NumberStyles.Float, Inv, out var parsed))
        {
            return parsed;
        }
        return null;
    }

    private static long? AsInt64(BsonValue value)
    {
        if (value.IsNumeric)
        {
            return value.ToInt64();
        }
        if (value.IsString && long.TryParse(value.AsString, NumberStyles.Integer, Inv, out var parsed))
        {
            return parsed;
        }
        return null;
    }
}
