using System.Text.Json.Nodes;

namespace BaklavaBackend.Common;

/// Turns a normalised scene (meta + ships, see SceneNormalizer) into the shape
/// the map frontend wants.
///
/// The one piece of real logic here is the footprint. The frontend draws every
/// scene as a box *before* anything is known about its vessels, so it needs the
/// scene's own four corners. The detector now writes them as
/// "footprint_lonlat"; for scenes produced before that, the bounding box of the
/// detections is used as a stand-in, which is smaller than the truth but is
/// better than no box at all.
public static class SceneProjector
{
    /// Four [lat, lon] corners for a scene, or null when neither the footprint
    /// nor any positioned detection is available.
    public static double[][]? Footprint(JsonObject normalized, JsonObject? rawCpp)
    {
        // 1. The detector's own footprint, written as [lon, lat] pairs.
        var fp = (rawCpp?["footprint_lonlat"] ?? normalized["footprint_lonlat"]) as JsonArray;
        if (fp is { Count: >= 4 })
        {
            var corners = new List<double[]>();
            foreach (var node in fp)
            {
                if (node is not JsonArray pair || pair.Count < 2) continue;
                var lon = pair[0]?.GetValue<double>();
                var lat = pair[1]?.GetValue<double>();
                if (lon is null || lat is null) continue;
                corners.Add(new[] { lat.Value, lon.Value });
            }
            if (corners.Count >= 4)
                return corners.Take(4).ToArray();
        }

        // 2. Fall back to the bounding box of whatever has a position.
        var ships = normalized["ships"] as JsonArray;
        if (ships is null || ships.Count == 0)
            return null;

        double minLat = double.MaxValue, maxLat = double.MinValue;
        double minLon = double.MaxValue, maxLon = double.MinValue;
        var any = false;
        foreach (var node in ships)
        {
            var lat = node?["latitude"]?.GetValue<double?>();
            var lon = node?["longitude"]?.GetValue<double?>();
            if (lat is null || lon is null) continue;
            any = true;
            minLat = Math.Min(minLat, lat.Value); maxLat = Math.Max(maxLat, lat.Value);
            minLon = Math.Min(minLon, lon.Value); maxLon = Math.Max(maxLon, lon.Value);
        }
        if (!any) return null;

        // A single detection would collapse to a point, which Leaflet draws as
        // nothing. Give it a small extent so the box stays clickable.
        if (maxLat - minLat < 1e-4) { minLat -= 5e-4; maxLat += 5e-4; }
        if (maxLon - minLon < 1e-4) { minLon -= 5e-4; maxLon += 5e-4; }

        return new[]
        {
            new[] { minLat, minLon },
            new[] { minLat, maxLon },
            new[] { maxLat, maxLon },
            new[] { maxLat, minLon },
        };
    }

    /// A human label for the scene picker. Scene names are long Sentinel-1
    /// filenames, so this pulls the mission and the acquisition date out of one
    /// rather than showing 70 characters of identifier.
    ///
    /// S1C_IW_GRDH_1SDV_20260807T041308_..._VH  ->  "S1C - 7 Aug 2026 04:13"
    public static string Label(string sceneId, JsonObject? meta)
    {
        var mission = sceneId.Length >= 3 ? sceneId[..3].ToUpperInvariant() : sceneId;
        if (mission is not ("S1A" or "S1B" or "S1C" or "S1D"))
            mission = string.Empty;

        DateTime? when = null;
        if (meta is not null && SceneNormalizer.TryGetAcquired(meta, out var acquired))
            when = acquired;

        if (when is null)
            return sceneId;

        var stamp = when.Value.ToString("d MMM yyyy HH:mm",
            System.Globalization.CultureInfo.InvariantCulture);
        return string.IsNullOrEmpty(mission) ? stamp : $"{mission} - {stamp}";
    }

    /// The vessels of a scene.
    ///
    /// `darkIds` is the set of detection ids that had NO AIS match, i.e. the
    /// dark ones. When AIS matching is switched off it is null, and every
    /// detection is reported dark - which is the honest answer: without AIS to
    /// compare against, nothing has been ruled out.
    public static List<VesselDto> Vessels(string sceneId, JsonObject normalized,
                                          IReadOnlySet<string>? darkIds)
    {
        var ships = normalized["ships"] as JsonArray;
        var meta = normalized["meta"] as JsonObject;
        var vessels = new List<VesselDto>();
        if (ships is null) return vessels;

        string? detectedAt = null;
        if (meta is not null && SceneNormalizer.TryGetAcquired(meta, out var acquired))
            detectedAt = acquired.ToString("yyyy-MM-dd HH:mm 'UTC'",
                System.Globalization.CultureInfo.InvariantCulture);

        foreach (var node in ships)
        {
            if (node is not JsonObject ship) continue;
            var lat = ship["latitude"]?.GetValue<double?>();
            var lon = ship["longitude"]?.GetValue<double?>();
            if (lat is null || lon is null) continue;

            var rawId = ship["id"]?.GetValue<int>() ?? vessels.Count + 1;
            var detectionId = $"{sceneId}-{rawId}";
            var dark = darkIds is null || darkIds.Contains(detectionId);

            var length = ship["length_m"]?.GetValue<double?>();
            var width = ship["width_m"]?.GetValue<double?>();

            vessels.Add(new VesselDto(
                Id: $"{sceneId}-{rawId:D3}",
                Lat: lat.Value,
                Lon: lon.Value,
                Dark: dark,
                // The detector sees a shape, not an identity. Naming a dark
                // contact anything more specific than what it is would be
                // inventing information.
                Name: dark ? $"Dark contact {rawId:D2}" : $"AIS-matched contact {rawId:D2}",
                Mmsi: "",
                Type: "Unknown",
                LengthM: length,
                // An oriented box carries no bow/stern, so this is an axis in
                // 0-180, not a course. Passed through as the detector gives it.
                HeadingDeg: ship["heading"]?.GetValue<double?>(),
                // Nothing in a single SAR frame measures speed.
                SpeedKn: null,
                DetectedAt: detectedAt,
                Confidence: ship["conf"]?.GetValue<double?>()));

            _ = width; // carried in the JSON, not shown on the card
        }

        return vessels;
    }
}
