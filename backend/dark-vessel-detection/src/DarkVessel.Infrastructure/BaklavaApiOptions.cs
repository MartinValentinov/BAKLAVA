namespace DarkVessel.Infrastructure;

/// <summary>Bound from configuration -- see HttpAisSource.</summary>
public sealed class BaklavaApiOptions
{
    public const string SectionName = "Baklava";

    /// <summary>Trailing slash matters -- requests are made relative to this.</summary>
    public string BaseUrl { get; set; } = "https://supm.online/api/";

    public string ApiKey { get; set; } = "";

    /// <summary>
    /// The timezone the API's <c>since</c>/<c>until</c> query parameters are read in.
    ///
    /// It is not UTC, even though the <c>ts</c> values the API hands back are: ask for
    /// <c>since=2026-08-07 04:00:00&amp;until=2026-08-07 05:00:00</c> and rows stamped
    /// <c>2026-08-07 04:13:00</c> do not come back -- they only appear once the bounds
    /// are moved to <c>07:13</c>, i.e. readers.php parses the bounds in the server's own
    /// local time before comparing them against a UTC column. Rather than hardcode the
    /// +3, name the zone: an IANA/Windows id keeps the conversion right across the
    /// summer/winter switch, and makes this a one-line change if the site ever moves
    /// host or fixes the bug (set it to "UTC" then).
    /// </summary>
    public string QueryTimeZone { get; set; } = "Europe/Sofia";
}