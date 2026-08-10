namespace DarkVessel.Infrastructure;

public sealed class BaklavaApiOptions
{
    public const string SectionName = "Baklava";

    public string BaseUrl { get; set; } = "https://supm.online/api/";

    public string ApiKey { get; set; } = "";

    public string QueryTimeZone { get; set; } = "Europe/Sofia";
}