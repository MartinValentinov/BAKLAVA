namespace DarkVessel.Core;

public sealed class InMemoryAisSource : IAisSource
{
    public List<AisPosition> Positions { get; } = new();

    public List<(DateTime StartedAt, DateTime EndedAt)> Coverage { get; } = new();

    public Task<IReadOnlyList<AisPosition>> CandidatesNearAsync(
        double lat, double lon, DateTime when, double windowMinutes, double radiusKm,
        CancellationToken ct = default)
    {
        var half = TimeSpan.FromMinutes(windowMinutes);
        var lo = when - half;
        var hi = when + half;

        IReadOnlyList<AisPosition> result = Positions
            .Where(p => p.Ts >= lo && p.Ts <= hi && GeoUtils.HaversineKm(lat, lon, p.Lat, p.Lon) <= radiusKm)
            .ToList();

        return Task.FromResult(result);
    }

    public Task<bool> HadCoverageAsync(DateTime when, double slackMinutes = 5.0, CancellationToken ct = default)
    {
        var slack = TimeSpan.FromMinutes(slackMinutes);
        var lo = when - slack;
        var hi = when + slack;
        bool covered = Coverage.Any(c => c.StartedAt <= hi && c.EndedAt >= lo);
        return Task.FromResult(covered);
    }
}
