using DarkVessel.Core;
using Xunit;

namespace DarkVessel.Core.Tests;

/// <summary>
/// Tests for Matcher, entirely against synthetic fixtures -- no database, no
/// network. Mirrors intelligence/test_matching.py scenario for scenario.
/// </summary>
public class MatcherTests
{
    private static readonly DateTime T0 = new(2026, 8, 1, 12, 0, 0, DateTimeKind.Utc);

    private static DateTime Minutes(double n) => T0.AddMinutes(n);

    private static InMemoryAisSource MakeSource(
        IEnumerable<AisPosition>? positions = null,
        IEnumerable<(DateTime, DateTime)>? coverage = null)
    {
        var source = new InMemoryAisSource();
        if (positions is not null)
        {
            source.Positions.AddRange(positions);
        }
        // Default: one open coverage window spanning an hour either side of T0.
        var cov = coverage ?? new[] { (T0.AddHours(-1), T0.AddHours(1)) };
        source.Coverage.AddRange(cov);
        return source;
    }

    [Fact]
    public async Task MatchedVessel_WithinThreshold()
    {
        // AIS reports the ship almost exactly where and when it was detected.
        var source = MakeSource(new[] { new AisPosition(123456789, T0, 40.000, 25.000) });
        var detection = new Detection("d1", 40.001, 25.001, T0);

        var result = await Matcher.MatchDetectionAsync(detection, source);

        Assert.Equal(MatchStatus.Matched, result.Status);
        Assert.NotNull(result.Best);
        Assert.Equal(123456789, result.Best!.Mmsi);
        Assert.True(result.Best.DistanceKm < 1.0);
    }

    [Fact]
    public async Task DarkVessel_WithActiveCoverage()
    {
        // Nothing on AIS anywhere near this detection, but the collector was running.
        var source = MakeSource(new[] { new AisPosition(999, T0, 10.0, 10.0) }); // far away
        var detection = new Detection("d2", 40.0, 25.0, T0);

        var result = await Matcher.MatchDetectionAsync(detection, source);

        Assert.Equal(MatchStatus.Dark, result.Status);
        Assert.True(result.HadCoverage);
        Assert.Null(result.Best);
    }

    [Fact]
    public async Task NoCoverage_IsNotDeclaredDark()
    {
        // Same "nothing nearby" picture, but the archive wasn't listening then --
        // this must NOT be reported as a dark vessel.
        var source = MakeSource(positions: Array.Empty<AisPosition>(), coverage: Array.Empty<(DateTime, DateTime)>());
        var detection = new Detection("d3", 40.0, 25.0, T0);

        var result = await Matcher.MatchDetectionAsync(detection, source);

        Assert.Equal(MatchStatus.UnknownNoCoverage, result.Status);
        Assert.False(result.HadCoverage);
    }

    [Fact]
    public async Task NearestCandidate_WinsAmongMultiple()
    {
        var source = MakeSource(new[]
        {
            new AisPosition(1, T0, 40.03, 25.03),    // further, still within radiusKm
            new AisPosition(2, T0, 40.002, 25.002),  // closer
        });
        var detection = new Detection("d4", 40.0, 25.0, T0);

        var result = await Matcher.MatchDetectionAsync(detection, source);

        Assert.Equal(MatchStatus.Matched, result.Status);
        Assert.Equal(2, result.Best!.Mmsi);
        // both candidates should be visible, nearest first, for the coast guard's own read
        Assert.Equal(new long[] { 2, 1 }, result.Candidates.Select(c => c.Mmsi));
    }

    [Fact]
    public async Task TimeGapTooLarge_ExcludesCandidate()
    {
        // Physically nearby, but the only AIS report is 40 minutes away in time --
        // too stale to trust as an explanation for this detection.
        var source = MakeSource(new[] { new AisPosition(1, Minutes(40), 40.0, 25.0) });
        var detection = new Detection("d5", 40.0, 25.0, T0);

        var result = await Matcher.MatchDetectionAsync(detection, source, maxTimeGapSeconds: 900);

        Assert.Equal(MatchStatus.Dark, result.Status);
        Assert.Null(result.Best);
    }

    [Fact]
    public async Task Interpolation_BetweenTwoBracketingReports()
    {
        // Vessel reported 10 min before and 10 min after the detection, moving in
        // a straight line; the detection sits exactly at the interpolated midpoint.
        // Both bracketing reports are kept within the default candidate radius of
        // the detection point -- a fast-moving vessel whose reports individually
        // fall outside radiusKm needs a wider radiusKm passed explicitly.
        var source = MakeSource(new[]
        {
            new AisPosition(1, Minutes(-10), 40.020, 25.020),
            new AisPosition(1, Minutes(10), 40.080, 25.080),
        });
        var detection = new Detection("d6", 40.050, 25.050, T0);

        var result = await Matcher.MatchDetectionAsync(detection, source);

        Assert.Equal(MatchStatus.Matched, result.Status);
        Assert.Equal(0.0, result.Best!.TimeGapSeconds); // bracketed, not extrapolated
        Assert.True(result.Best.DistanceKm < 0.1);
    }

    [Fact]
    public async Task RejectsNonUtcTimestamp()
    {
        var source = MakeSource();
        var detection = new Detection("d7", 0.0, 0.0, new DateTime(2026, 8, 1, 12, 0, 0, DateTimeKind.Unspecified));

        await Assert.ThrowsAsync<ArgumentException>(() => Matcher.MatchDetectionAsync(detection, source));
    }
}
