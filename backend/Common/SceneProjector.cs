using System.Text.Json.Nodes;

namespace BaklavaBackend.Common;

public static class SceneProjector
{
    public static List<TimingStage>? Timings(JsonObject? rawCpp)
    {
        if (rawCpp?["timings"] is not JsonArray stages || stages.Count == 0)
            return null;

        var list = new List<TimingStage>();
        foreach (var node in stages)
        {
            if (node is not JsonObject stage) continue;
            var name = stage["stage"]?.GetValue<string>();
            var ms = stage["ms"]?.GetValue<double>();
            if (name is null || ms is null) continue;
            list.Add(new TimingStage(name, ms.Value));
        }

        return list.Count > 0 ? list : null;
    }

    public static double[][]? Swath(JsonObject? rawCpp) => LonLatQuad(rawCpp, "swath_lonlat");

    public static double[][]? OverviewBounds(JsonObject? rawCpp) =>
        LonLatQuad(rawCpp, "overview_bounds_lonlat");

    private static double[][]? LonLatQuad(JsonObject? rawCpp, string key)
    {
        if (rawCpp?[key] is not JsonArray bounds || bounds.Count < 4)
            return null;

        var corners = new List<double[]>();
        foreach (var node in bounds)
        {
            if (node is not JsonArray pair || pair.Count < 2) continue;
            var lon = pair[0]?.GetValue<double>();
            var lat = pair[1]?.GetValue<double>();
            if (lon is null || lat is null) continue;
            corners.Add(new[] { lat.Value, lon.Value });
        }

        return corners.Count >= 4 ? corners.Take(4).ToArray() : null;
    }

    public static double[][]? Footprint(JsonObject normalized, JsonObject? rawCpp)
    {
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

            double[][]? outline = null;
            if (ship["corners"] is JsonArray corners && corners.Count > 0)
            {
                outline = corners
                    .Select(c => new[] { c![0]!.GetValue<double>(), c![1]!.GetValue<double>() })
                    .ToArray();
            }

            vessels.Add(new VesselDto(
                Id: $"{sceneId}-{rawId:D3}",
                Lat: lat.Value,
                Lon: lon.Value,
                Dark: dark,
                Name: dark ? $"Dark contact {rawId:D2}" : $"AIS-matched contact {rawId:D2}",
                Mmsi: "",
                Type: "Unknown",
                LengthM: length,
                HeadingDeg: ship["heading"]?.GetValue<double?>(),
                SpeedKn: null,
                DetectedAt: detectedAt,
                Confidence: ship["conf"]?.GetValue<double?>(),
                Corners: outline));

            _ = width;
        }

        return vessels;
    }
}
