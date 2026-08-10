using System.Text.Json.Serialization;

namespace BaklavaBackend.Common;

/// The shapes the map frontend (new_frontend/) consumes. Its contract is
/// written down in new_frontend/README.md under "Backend contract"; these
/// records are that contract in C#, so a rename here that is not mirrored there
/// is a compile-visible change rather than a silently empty map.
///
/// Coordinates are [lat, lon] pairs, in that order, because that is the order
/// Leaflet takes them in and the frontend hands them straight to it.

/// One entry of GET /api/scenes - a blue box on the map.
public record SceneSummary(
    [property: JsonPropertyName("id")] string Id,
    [property: JsonPropertyName("label")] string Label,
    [property: JsonPropertyName("corners")] double[][] Corners,
    [property: JsonPropertyName("has_sar")] bool HasSar);

public record SceneListResponse(
    [property: JsonPropertyName("scenes")] IReadOnlyList<SceneSummary> Scenes);

/// One vessel inside a scene. Every field except Lat, Lon and Dark is optional
/// on the frontend: a null simply leaves that row off the details card.
public record VesselDto(
    [property: JsonPropertyName("id")] string Id,
    [property: JsonPropertyName("lat")] double Lat,
    [property: JsonPropertyName("lon")] double Lon,
    [property: JsonPropertyName("dark")] bool Dark,
    [property: JsonPropertyName("name")] string? Name,
    [property: JsonPropertyName("mmsi")] string? Mmsi,
    [property: JsonPropertyName("type")] string? Type,
    [property: JsonPropertyName("length_m")] double? LengthM,
    [property: JsonPropertyName("heading_deg")] double? HeadingDeg,
    [property: JsonPropertyName("speed_kn")] double? SpeedKn,
    [property: JsonPropertyName("detected_at")] string? DetectedAt,
    [property: JsonPropertyName("confidence")] double? Confidence);

public record SceneTotals(
    [property: JsonPropertyName("total")] int Total,
    [property: JsonPropertyName("dark")] int Dark);

/// The radar picture laid over the map. Corners are the four corners of the
/// picture, which for our overview render are the scene's own.
public record SarOverlay(
    [property: JsonPropertyName("url")] string Url,
    [property: JsonPropertyName("corners")] double[][] Corners);

/// GET /api/scenes/{id}
public record SceneDetail(
    [property: JsonPropertyName("id")] string Id,
    [property: JsonPropertyName("label")] string Label,
    [property: JsonPropertyName("corners")] double[][] Corners,
    [property: JsonPropertyName("totals")] SceneTotals Totals,
    [property: JsonPropertyName("vessels")] IReadOnlyList<VesselDto> Vessels,
    [property: JsonPropertyName("sar_overlay")] SarOverlay? SarOverlay,
    // Extras beyond the frontend's required contract. It ignores unknown keys,
    // so these are additive: the water crops produced by the detector, and
    // whether a full-resolution set exists to pull.
    [property: JsonPropertyName("crops")] CropSummary? Crops);

/// What the detector wrote into <scene>_crops/manifest.json, summarised.
public record CropSummary(
    [property: JsonPropertyName("count")] int Count,
    [property: JsonPropertyName("crop_size")] int CropSize,
    [property: JsonPropertyName("thumb_size")] int ThumbSize,
    [property: JsonPropertyName("bytes_full")] long BytesFull,
    [property: JsonPropertyName("bytes_thumb")] long BytesThumb,
    [property: JsonPropertyName("manifest_url")] string ManifestUrl);
