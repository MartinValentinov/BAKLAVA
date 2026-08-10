namespace DarkVessel.Core;

public enum MatchStatus
{
    Matched,
    Dark,
    UnknownNoCoverage,
}

public sealed record Detection(
    string DetectionId,
    double Lat,
    double Lon,
    DateTime Timestamp,
    double? HeadingDeg = null,
    double? LengthM = null,
    double? WidthM = null);

public sealed record AisPosition(
    long Mmsi,
    DateTime Ts,
    double Lat,
    double Lon,
    double? Sog = null,
    double? Cog = null,
    double? Heading = null);

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
    IReadOnlyList<Candidate> Candidates);
