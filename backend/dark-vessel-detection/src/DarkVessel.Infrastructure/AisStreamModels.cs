using System.Text.Json.Serialization;

namespace DarkVessel.Infrastructure;

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
    [JsonPropertyName("A")]
    public double? A { get; set; }

    [JsonPropertyName("B")]
    public double? B { get; set; }

    [JsonPropertyName("C")]
    public double? C { get; set; }

    [JsonPropertyName("D")]
    public double? D { get; set; }
}

public sealed class AisStreamSubscription
{
    [JsonPropertyName("APIKey")]
    public string ApiKey { get; set; } = "";

    [JsonPropertyName("BoundingBoxes")]
    public double[][][] BoundingBoxes { get; set; } = [];

    [JsonPropertyName("FilterMessageTypes")]
    public string[] FilterMessageTypes { get; set; } = [];
}
