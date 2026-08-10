namespace DarkVessel.Core;

public interface IAisSource
{
    Task<IReadOnlyList<AisPosition>> CandidatesNearAsync(
        double lat, double lon, DateTime when, double windowMinutes, double radiusKm,
        CancellationToken ct = default);

    Task<bool> HadCoverageAsync(DateTime when, double slackMinutes = 5.0, CancellationToken ct = default);
}
