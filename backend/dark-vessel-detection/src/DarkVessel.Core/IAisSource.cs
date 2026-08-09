namespace DarkVessel.Core;

/// <summary>What the matcher needs from wherever AIS data actually lives.</summary>
public interface IAisSource
{
    /// <summary>
    /// All AIS positions within <paramref name="radiusKm"/> of (lat, lon), across all
    /// vessels, reported within <paramref name="windowMinutes"/> on either side of <paramref name="when"/>.
    /// </summary>
    Task<IReadOnlyList<AisPosition>> CandidatesNearAsync(
        double lat, double lon, DateTime when, double windowMinutes, double radiusKm,
        CancellationToken ct = default);

    /// <summary>
    /// Was the archive actually listening around <paramref name="when"/>?
    ///
    /// This is what stops a collector outage from manufacturing dark vessels:
    /// "no AIS nearby" only means DARK if this returns true.
    /// </summary>
    Task<bool> HadCoverageAsync(DateTime when, double slackMinutes = 5.0, CancellationToken ct = default);
}
