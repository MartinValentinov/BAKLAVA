using DarkVessel.Core;

namespace BaklavaBackend.Services;

public sealed record ShipDetection(string DetectionId, double Lat, double Lon, double? HeadingDeg);

public class DarkVesselMatchService
{
    private readonly IAisSource? _ais;
    private readonly int _maxConcurrentMatches;
    private readonly ILogger<DarkVesselMatchService> _logger;

    public bool Enabled => _ais is not null;

    public DarkVesselMatchService(IAisSource? ais, IConfiguration config, ILogger<DarkVesselMatchService> logger)
    {
        _ais = ais;
        _logger = logger;

        if (_ais is null)
            _logger.LogWarning(
                "the AIS archive is not configured (Mongo:ConnectionString is blank); "
                + "scenes will return every detected ship, unfiltered");

        _maxConcurrentMatches = int.TryParse(config["DarkVessel:MaxConcurrentMatches"], out var m) ? m : 8;
    }

    private IAisSource EnsureEnabled()
        => _ais ?? throw new InvalidOperationException(
            "the AIS archive is not configured; check Enabled before calling this");

    public async Task<MatchStatus> MatchStatusAsync(
        string detectionId, double lat, double lon, DateTime timestampUtc, double? headingDeg, CancellationToken ct)
    {
        var ais = EnsureEnabled();
        var detection = new Detection(
            detectionId, lat, lon, DateTime.SpecifyKind(timestampUtc, DateTimeKind.Utc), headingDeg);

        var result = await Matcher.MatchDetectionAsync(detection, ais, ct: ct);
        return result.Status;
    }

    public async Task<HashSet<string>> FilterKeepAsync(IReadOnlyList<ShipDetection> ships, DateTime timestampUtc, CancellationToken ct)
    {
        EnsureEnabled();
        var keep = new HashSet<string>();
        var counts = new Dictionary<MatchStatus, int>();
        var gate = new object();

        await Parallel.ForEachAsync(ships, new ParallelOptions { MaxDegreeOfParallelism = _maxConcurrentMatches, CancellationToken = ct }, async (ship, token) =>
        {
            var status = await MatchStatusAsync(ship.DetectionId, ship.Lat, ship.Lon, timestampUtc, ship.HeadingDeg, token);
            lock (gate)
            {
                counts[status] = counts.GetValueOrDefault(status) + 1;
                if (status != MatchStatus.Matched)
                    keep.Add(ship.DetectionId);
            }
        });

        _logger.LogInformation("dark-vessel match over {Total} ships: {Breakdown}",
            ships.Count, string.Join(", ", counts.Select(kv => $"{kv.Key}={kv.Value}")));

        return keep;
    }
}
