namespace DarkVessel.Core;

/// <summary>
/// Cross-match onboard ship detections against the live AIS archive.
///
/// The onboard model reports a position and timestamp (and, where the modality
/// allows it, a heading) for every detected vessel. This answers the one
/// question that matters operationally: is there an AIS-reporting vessel close
/// enough, in space and time, to explain the detection? If yes, the ship is
/// transparent -- not dark. If no, and the archive was actually listening at
/// that moment (see <see cref="IAisSource.HadCoverageAsync"/>), it's a dark vessel.
///
/// This is a simple distance+time gate, not a weighted score: a candidate only
/// counts if it is within <c>matchDistanceKm</c> and <c>maxTimeGapSeconds</c> of
/// the detection. That keeps the decision easy to explain to a coast guard
/// reviewer who is not going to trust an opaque probability. Every candidate
/// considered -- not just the winner -- is returned, so a human can see what
/// almost matched before deciding whether a "dark" verdict is worth acting on.
/// </summary>
public static class Matcher
{
    public static async Task<MatchResult> MatchDetectionAsync(
        Detection detection,
        IAisSource source,
        double windowMinutes = 20.0,
        double radiusKm = 5.0,
        double matchDistanceKm = 1.0,
        double maxTimeGapSeconds = 900.0,
        CancellationToken ct = default)
    {
        if (detection.Timestamp.Kind != DateTimeKind.Utc)
        {
            throw new ArgumentException("detection.Timestamp must be UTC (DateTimeKind.Utc)", nameof(detection));
        }

        bool hadCoverage = await source.HadCoverageAsync(detection.Timestamp, ct: ct).ConfigureAwait(false);

        var positions = await source
            .CandidatesNearAsync(detection.Lat, detection.Lon, detection.Timestamp, windowMinutes, radiusKm, ct)
            .ConfigureAwait(false);

        var tracks = new Dictionary<long, List<AisPosition>>();
        foreach (var p in positions)
        {
            if (!tracks.TryGetValue(p.Mmsi, out var track))
            {
                track = new List<AisPosition>();
                tracks[p.Mmsi] = track;
            }
            track.Add(p);
        }

        var candidates = new List<Candidate>();
        foreach (var (mmsi, track) in tracks)
        {
            var (lat, lon, gapSeconds) = Interpolate(track, detection.Timestamp);
            if (gapSeconds > maxTimeGapSeconds)
            {
                continue;
            }
            candidates.Add(new Candidate(
                Mmsi: mmsi,
                DistanceKm: GeoUtils.HaversineKm(detection.Lat, detection.Lon, lat, lon),
                TimeGapSeconds: gapSeconds,
                InterpolatedLat: lat,
                InterpolatedLon: lon));
        }
        candidates.Sort((a, b) => a.DistanceKm.CompareTo(b.DistanceKm));

        Candidate? best = candidates.Count > 0 ? candidates[0] : null;

        MatchStatus status;
        if (best is not null && best.DistanceKm <= matchDistanceKm)
        {
            status = MatchStatus.Matched;
        }
        else if (!hadCoverage)
        {
            status = MatchStatus.UnknownNoCoverage;
        }
        else
        {
            status = MatchStatus.Dark;
        }

        return new MatchResult(detection.DetectionId, status, hadCoverage, best, candidates);
    }

    /// <summary>
    /// One vessel's position at <paramref name="when"/>.
    ///
    /// Linearly interpolated between the two AIS reports bracketing <paramref name="when"/>
    /// if both exist (the collector stores one position per vessel per minute, so this
    /// keeps a 14-knot vessel's true position within ~100 m of the interpolated one).
    /// Falls back to the single nearest report -- with its real time gap reported --
    /// when <paramref name="when"/> is outside the track's span.
    /// </summary>
    private static (double Lat, double Lon, double GapSeconds) Interpolate(List<AisPosition> track, DateTime when)
    {
        var sorted = track.OrderBy(p => p.Ts).ToList();
        var before = sorted.Where(p => p.Ts <= when).ToList();
        var after = sorted.Where(p => p.Ts >= when).ToList();

        if (before.Count > 0 && after.Count > 0 && before[^1].Ts != after[0].Ts)
        {
            var p0 = before[^1];
            var p1 = after[0];
            double spanSeconds = (p1.Ts - p0.Ts).TotalSeconds;
            double frac = (when - p0.Ts).TotalSeconds / spanSeconds;
            double lat = p0.Lat + (p1.Lat - p0.Lat) * frac;
            double lon = p0.Lon + (p1.Lon - p0.Lon) * frac;
            return (lat, lon, 0.0);
        }

        var nearest = sorted.MinBy(p => Math.Abs((p.Ts - when).TotalSeconds))!;
        return (nearest.Lat, nearest.Lon, Math.Abs((nearest.Ts - when).TotalSeconds));
    }
}
