using System.Globalization;
using System.Text.RegularExpressions;
using System.Text.Json.Nodes;

namespace BaklavaBackend.Common;

/// Brings both scene formats the Jetson can emit onto one shape - meta + ships -
/// so the dark-vessel filter and the frontend only ever deal with one.
///
///   legacy  txt_to_json.py       meta{...} + ships[{id,conf,latitude,longitude,
///                                heading,length_m,width_m,col_px,row_px}]
///   cpp     yolov8s-obb-cpp      scene, acquisition_time, detection_count,
///                                detections[{confidence,center{lon,lat},
///                                heading_deg,length_m,width_m,corners_pixel}]
///
/// listener_service.py writes the cpp result under a __cpp.json name and tags it
/// "backend": "yolov8s-obb-cpp", so both can sit in the outbox at once.
public static partial class SceneNormalizer
{
    [GeneratedRegex(@"\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?Z?")]
    private static partial Regex IsoStamp();

    /// True when the object is already legacy-shaped.
    public static bool IsLegacy(JsonObject root) =>
        root["meta"] is JsonObject && root["ships"] is JsonArray;

    /// True when the object is the cpp backend's shape.
    public static bool IsCpp(JsonObject root) => root["detections"] is JsonArray;

    /// cpp shape -> legacy shape. Returns the input untouched if it is not cpp.
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

            // corners_pixel is four [x,y] corners; the frontend wants one point,
            // so use their centroid. Absent on some outputs, hence the null.
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

            ships.Add(new JsonObject
            {
                // cpp detections carry no id of their own; position in the list
                // is the only stable handle, and it is what the detection ids
                // sent to DarkVessel.Api are built from.
                ["id"] = i + 1,
                ["conf"] = d["confidence"]?.GetValue<double>(),
                ["longitude"] = centre?["lon"]?.GetValue<double>(),
                ["latitude"] = centre?["lat"]?.GetValue<double>(),
                ["length_m"] = d["length_m"]?.GetValue<double>(),
                ["width_m"] = d["width_m"]?.GetValue<double>(),
                ["heading"] = d["heading_deg"]?.GetValue<double>(),
                ["col_px"] = colPx,
                ["row_px"] = rowPx,
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

    /// The acquisition time out of meta.acquired.
    ///
    /// detect_ships.py writes provenance into that field rather than a bare
    /// timestamp - "2026-08-07T04:13:08Z  (from scene filename)", or
    /// "unknown - name the gpt output after..." when it could not find one. So
    /// the whole string never parses; the leading ISO stamp is pulled out
    /// instead, and a scene with no timestamp at all returns false rather than
    /// failing the request.
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
