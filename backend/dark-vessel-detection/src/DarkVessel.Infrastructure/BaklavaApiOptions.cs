namespace DarkVessel.Infrastructure;

/// <summary>Bound from configuration -- see HttpAisSource.</summary>
public sealed class BaklavaApiOptions
{
    public const string SectionName = "Baklava";

    /// <summary>Trailing slash matters -- requests are made relative to this.</summary>
    public string BaseUrl { get; set; } = "https://supm.online/api/";

    public string ApiKey { get; set; } = "";
}