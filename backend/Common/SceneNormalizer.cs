using System.Globalization;
using System.Text.RegularExpressions;
using System.Text.Json.Nodes;

namespace BaklavaBackend.Common;

public static partial class SceneNormalizer
{
    [GeneratedRegex(@"\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?Z?")]
    private static partial Regex IsoStamp();

    public static bool IsLegacy(JsonObject root) =>
        root["meta"] is JsonObject && root["ships"] is JsonArray;

    public static bool IsCpp(JsonObject root) => root["detections"] is JsonArray;

    public static JsonObject Normalize(JsonObject root)
    {
        if (!IsCpp(root))
            return root;

        var detections = root["detections"]!.AsArray();
        var ships = new JsonArray();

        for (var i = 0; i < detections.Count; i++)
        {
            var d = detections[i]!.AsObject();
            var centre = d["center"]?.AsObject();

            double? colPx = null, rowPx = null;
            if (d["corners_pixel"] is JsonArray corners && corners.Count > 0)
            {
                double sx = 0, sy = 0;
                foreach (var c in corners)
                {
                    sx += c![0]!.GetValue<double>();
                    sy += c![1]!.GetValue<double>();
                }
                colPx = sx / corners.Count;
                rowPx = sy / corners.Count;
            }

            JsonArray? outline = null;
            if (d["corners_lonlat"] is JsonArray lonlat && lonlat.Count > 0)
            {
                outline = new JsonArray();
                foreach (var c in lonlat)
                {
                    outline.Add(new JsonArray(
                        c![1]!.GetValue<double>(),
                        c![0]!.GetValue<double>()));
                }
            }

            ships.Add(new JsonObject
            {
                ["id"] = i + 1,
                ["conf"] = d["confidence"]?.GetValue<double>(),
                ["longitude"] = centre?["lon"]?.GetValue<double>(),
                ["latitude"] = centre?["lat"]?.GetValue<double>(),
                ["length_m"] = d["length_m"]?.GetValue<double>(),
                ["width_m"] = d["width_m"]?.GetValue<double>(),
                ["heading"] = d["heading_deg"]?.GetValue<double>(),
                ["col_px"] = colPx,
                ["row_px"] = rowPx,
                ["corners"] = outline,
            });
        }

        return new JsonObject
        {
            ["meta"] = new JsonObject
            {
                ["scene"] = root["scene"]?.GetValue<string>(),
                ["acquired"] = root["acquisition_time"]?.GetValue<string>(),
                ["model"] = root["backend"]?.GetValue<string>() ?? "yolov8s-obb-cpp",
                ["ships"] = ships.Count,
            },
            ["ships"] = ships,
        };
    }

    public static bool TryGetAcquired(JsonObject meta, out DateTime acquiredUtc)
    {
        acquiredUtc = default;
        var raw = meta["acquired"]?.GetValue<string>();
        if (string.IsNullOrWhiteSpace(raw))
            return false;

        var match = IsoStamp().Match(raw);
        var candidate = match.Success ? match.Value : raw;

        return DateTime.TryParse(candidate, CultureInfo.InvariantCulture,
            DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out acquiredUtc);
    }
}
