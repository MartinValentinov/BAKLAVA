using System.Text.Json.Serialization;

namespace DarkVessel.Infrastructure;

// Field names match aisstream.io's documented schema (confirmed against
// aisstream_demo.ipynb for MetaData/PositionReport). Before relying on this
// against a live feed, cross-check a real captured payload -- the PHP
// collector logs the first raw payload of every session for exactly this
// reason ("[link] first payload: ..."), and aisstream can revise its schema.

public sealed class AisStreamEnvelope
{
    [JsonPropertyName("MessageType")]
    public string MessageType { get; set; } = "";

    [JsonPropertyName("MetaData")]
    public AisStreamMetaData? MetaData { get; set; }

    [JsonPropertyName("Message")]
    public AisStreamMessage? Message { get; set; }
}

public sealed class AisStreamMetaData
{
    [JsonPropertyName("MMSI")]
    public long Mmsi { get; set; }

    [JsonPropertyName("ShipName")]
    public string? ShipName { get; set; }

    [JsonPropertyName("time_utc")]
    public string? TimeUtc { get; set; }
}

public sealed class AisStreamMessage
{
    [JsonPropertyName("PositionReport")]
    public PositionReport? PositionReport { get; set; }

    [JsonPropertyName("ShipStaticData")]
    public ShipStaticData? ShipStaticData { get; set; }
}

public sealed class PositionReport
{
    [JsonPropertyName("Latitude")]
    public double Latitude { get; set; }

    [JsonPropertyName("Longitude")]
    public double Longitude { get; set; }

    [JsonPropertyName("Sog")]
    public double? Sog { get; set; }

    [JsonPropertyName("Cog")]
    public double? Cog { get; set; }

    [JsonPropertyName("TrueHeading")]
    public double? TrueHeading { get; set; }

    [JsonPropertyName("NavigationalStatus")]
    public int? NavigationalStatus { get; set; }
}

public sealed class ShipStaticData
{
    [JsonPropertyName("Name")]
    public string? Name { get; set; }

    [JsonPropertyName("CallSign")]
    public string? CallSign { get; set; }

    [JsonPropertyName("ImoNumber")]
    public long? ImoNumber { get; set; }

    [JsonPropertyName("Type")]
    public int? Type { get; set; }

    [JsonPropertyName("Dimension")]
    public ShipDimension? Dimension { get; set; }
}

public sealed class ShipDimension
{
    // Distances from the GPS antenna to bow(A)/stern(B)/port(C)/starboard(D).
    // length_m = A + B, width_m = C + D.
    [JsonPropertyName("A")]
    public double? A { get; set; }

    [JsonPropertyName("B")]
    public double? B { get; set; }

    [JsonPropertyName("C")]
    public double? C { get; set; }

    [JsonPropertyName("D")]
    public double? D { get; set; }
}

/// <summary>The subscribe frame sent once, right after the WebSocket opens.</summary>
public sealed class AisStreamSubscription
{
    [JsonPropertyName("APIKey")]
    public string ApiKey { get; set; } = "";

    [JsonPropertyName("BoundingBoxes")]
    public double[][][] BoundingBoxes { get; set; } = [];

    [JsonPropertyName("FilterMessageTypes")]
    public string[] FilterMessageTypes { get; set; } = [];
}
