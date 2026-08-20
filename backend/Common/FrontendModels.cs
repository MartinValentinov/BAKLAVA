using System.Text.Json.Serialization;

namespace BaklavaBackend.Common;

public record SceneSummary(
    [property: JsonPropertyName("id")] string Id,
    [property: JsonPropertyName("label")] string Label,
    [property: JsonPropertyName("corners")] double[][] Corners,
    [property: JsonPropertyName("has_sar")] bool HasSar);

public record SceneListResponse(
    [property: JsonPropertyName("scenes")] IReadOnlyList<SceneSummary> Scenes);

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
    [property: JsonPropertyName("heading_confidence")] double? HeadingConfidence,
    [property: JsonPropertyName("speed_kn")] double? SpeedKn,
    [property: JsonPropertyName("detected_at")] string? DetectedAt,
    [property: JsonPropertyName("confidence")] double? Confidence,
    [property: JsonPropertyName("corners")] double[][]? Corners);

public record SceneTotals(
    [property: JsonPropertyName("total")] int Total,
    [property: JsonPropertyName("dark")] int Dark);

public record SarOverlay(
    [property: JsonPropertyName("url")] string Url,
    [property: JsonPropertyName("corners")] double[][] Corners,
    [property: JsonPropertyName("swath")] double[][]? Swath);

public record TimingStage(
    [property: JsonPropertyName("stage")] string Stage,
    [property: JsonPropertyName("ms")] double Ms);

public record SceneDetail(
    [property: JsonPropertyName("id")] string Id,
    [property: JsonPropertyName("label")] string Label,
    [property: JsonPropertyName("corners")] double[][] Corners,
    [property: JsonPropertyName("totals")] SceneTotals Totals,
    [property: JsonPropertyName("vessels")] IReadOnlyList<VesselDto> Vessels,
    [property: JsonPropertyName("sar_overlay")] SarOverlay? SarOverlay,
    [property: JsonPropertyName("timings")] IReadOnlyList<TimingStage>? Timings);
