namespace DarkVessel.Core;

public enum MatchStatus
{
    Matched,
    Dark,
    UnknownNoCoverage,
}

/// <summary>One onboard-reported vessel position, ready to be checked against AIS.</summary>
/// <param name="Timestamp">Must be UTC (DateTimeKind.Utc).</param>
/// <param name="HeadingDeg">Onboard movement vector, if the modality provides one.</param>
public sealed record Detection(
    string DetectionId,
    double Lat,
    double Lon,
    DateTime Timestamp,
    double? HeadingDeg = null,
    double? LengthM = null,
    double? WidthM = null);

/// <summary>One AIS report. <paramref name="Ts"/> must be UTC.</summary>
public sealed record AisPosition(
    long Mmsi,
    DateTime Ts,
    double Lat,
    double Lon,
    double? Sog = null,
    double? Cog = null,
    double? Heading = null);

/// <summary>One AIS-reporting vessel considered as a possible explanation for a detection.</summary>
public sealed record Candidate(
    long Mmsi,
    double DistanceKm,
    double TimeGapSeconds,
    double InterpolatedLat,
    double InterpolatedLon);

public sealed record MatchResult(
    string DetectionId,
    MatchStatus Status,
    bool HadCoverage,
    Candidate? Best,
    // Nearest-first. Kept even when empty/non-matching, so a dark-vessel alert
    // can show the coast guard "closest AIS traffic was 4.2 km / 11 min away".
    IReadOnlyList<Candidate> Candidates);
