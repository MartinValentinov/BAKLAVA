namespace DarkVessel.Infrastructure;

public sealed class AisStreamOptions
{
    public const string SectionName = "AisStream";

    public string ApiKey { get; set; } = "";

    public double[][][] BoundingBoxes { get; set; } =
    [
        [[30.0, -6.5], [46.0, 36.5]],
        [[40.5, 26.5], [47.5, 42.0]],
    ];

    public string[] MessageTypes { get; set; } = ["PositionReport", "ShipStaticData"];

    public int FlushSeconds { get; set; } = 10;
    public int IdleReconnectSeconds { get; set; } = 120;

    public string ConnectionString { get; set; } = "";
}
