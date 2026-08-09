namespace DarkVessel.Infrastructure;

/// <summary>Bound from configuration (appsettings.json / user-secrets / env vars), mirrors ais_web/config.php.</summary>
public sealed class AisStreamOptions
{
    public const string SectionName = "AisStream";

    public string ApiKey { get; set; } = "";

    /// <summary>[[south, west], [north, east]] pairs -- exactly aisstream.io's BoundingBoxes shape.</summary>
    public double[][][] BoundingBoxes { get; set; } =
    [
        [[30.0, -6.5], [46.0, 36.5]],   // Mediterranean
        [[40.5, 26.5], [47.5, 42.0]],   // Black Sea
    ];

    public string[] MessageTypes { get; set; } = ["PositionReport", "ShipStaticData"];

    public int FlushSeconds { get; set; } = 10;
    public int IdleReconnectSeconds { get; set; } = 120;

    public string ConnectionString { get; set; } = "";
}
