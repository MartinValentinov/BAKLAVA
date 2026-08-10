using System.Text.Json.Nodes;

namespace BaklavaBackend.Common;

public static class SceneProjector
{
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
                Confidence: ship["conf"]?.GetValue<double?>()));

            _ = width;
        }

        return vessels;
    }
}
