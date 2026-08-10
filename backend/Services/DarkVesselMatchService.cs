using System.Text.Json;
using System.Text.Json.Serialization;

namespace BaklavaBackend.Services;

/// <summary>
/// Calls the separate DarkVessel.Api service (cross-references an onboard
/// detection against the AIS archive) to decide whether a detected ship is
/// a "dark vessel" -- see that project's API_REFERENCE.md. Field names are
/// explicit camelCase via [JsonPropertyName] rather than relying on default
/// JsonSerializerOptions, since a silent casing mismatch here would make
/// every ship read as "not dark" with no error anywhere.
/// </summary>
public sealed record MatchRequestDto(
    [property: JsonPropertyName("detectionId")] string DetectionId,
    [property: JsonPropertyName("lat")] double Lat,
    [property: JsonPropertyName("lon")] double Lon,
    [property: JsonPropertyName("timestampUtc")] DateTime TimestampUtc,
    [property: JsonPropertyName("headingDeg")] double? HeadingDeg);

public sealed record MatchResultDto(
    [property: JsonPropertyName("detectionId")] string DetectionId,
    [property: JsonPropertyName("status")] string Status);

public sealed record ShipDetection(string DetectionId, double Lat, double Lon, double? HeadingDeg);

public class DarkVesselMatchService
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    private readonly HttpClient _http;
    private readonly int _maxConcurrentMatches;
    private readonly ILogger<DarkVesselMatchService> _logger;

    /// False when DarkVessel:BaseUrl or DarkVessel:ApiKey is unset. The
    /// integration is optional: without it scenes come back with every
    /// detected ship instead of only the dark ones, rather than the whole
    /// ScenesController failing to construct.
    public bool Enabled { get; }

    public DarkVesselMatchService(HttpClient http, IConfiguration config, ILogger<DarkVesselMatchService> logger)
    {
        _http = http;
        _logger = logger;

        var baseUrl = config["DarkVessel:BaseUrl"];
        var apiKey = config["DarkVessel:ApiKey"];
        Enabled = !string.IsNullOrWhiteSpace(baseUrl) && !string.IsNullOrWhiteSpace(apiKey);

        if (Enabled)
            _http.DefaultRequestHeaders.Add("X-Api-Key", apiKey);
        else
            _logger.LogWarning(
                "DarkVessel is not configured (DarkVessel:BaseUrl / DarkVessel:ApiKey are blank); "
                + "scenes will return every detected ship, unfiltered");

        _maxConcurrentMatches = int.TryParse(config["DarkVessel:MaxConcurrentMatches"], out var m) ? m : 8;
    }

    private void EnsureEnabled()
    {
        if (!Enabled)
            throw new InvalidOperationException(
                "DarkVessel is not configured; check Enabled before calling this");
    }

    /// Raw MatchStatus from DarkVessel.Api: "Matched", "Dark" or "UnknownNoCoverage".
    public async Task<string> MatchStatusAsync(string detectionId, double lat, double lon, DateTime timestampUtc, double? headingDeg, CancellationToken ct)
    {
        EnsureEnabled();
        var request = new MatchRequestDto(detectionId, lat, lon, timestampUtc, headingDeg);

        using var response = await _http.PostAsJsonAsync("/api/match", request, JsonOptions, ct);
        if (!response.IsSuccessStatusCode)
        {
            var body = await response.Content.ReadAsStringAsync(ct);
            throw new HttpRequestException($"DarkVessel.Api /api/match returned {(int)response.StatusCode}: {body}");
        }

        var result = await response.Content.ReadFromJsonAsync<MatchResultDto>(JsonOptions, ct)
            ?? throw new HttpRequestException("DarkVessel.Api /api/match returned an empty body");

        return result.Status;
    }

    /// <summary>
    /// Matches many ships concurrently, bounded by DarkVessel:MaxConcurrentMatches
    /// (default 8) -- each call fans out to two further HTTP calls inside
    /// DarkVessel.Api, so an unbounded Task.WhenAll over a whole scene isn't safe.
    ///
    /// Returns the DetectionIds worth showing: everything except "Matched".
    ///
    /// Only "Matched" is a positive identification -- the vessel was found in the
    /// AIS archive, so it is cooperative and not of interest. "Dark" and
    /// "UnknownNoCoverage" are both kept, and they are NOT the same thing:
    /// "Dark" means AIS was being recorded there and then and this ship was
    /// absent; "UnknownNoCoverage" means nobody was listening, so nothing can be
    /// concluded. Dropping the unknowns would silently hide real contacts
    /// whenever AIS coverage has a hole -- which, until a coverage resource
    /// exists on writers.php, is every scene.
    /// </summary>
    public async Task<HashSet<string>> FilterKeepAsync(IReadOnlyList<ShipDetection> ships, DateTime timestampUtc, CancellationToken ct)
    {
        EnsureEnabled();
        var keep = new HashSet<string>();
        var counts = new Dictionary<string, int>();
        var gate = new object();

        await Parallel.ForEachAsync(ships, new ParallelOptions { MaxDegreeOfParallelism = _maxConcurrentMatches, CancellationToken = ct }, async (ship, token) =>
        {
            var status = await MatchStatusAsync(ship.DetectionId, ship.Lat, ship.Lon, timestampUtc, ship.HeadingDeg, token);
            lock (gate)
            {
                counts[status] = counts.GetValueOrDefault(status) + 1;
                if (status != "Matched")
                    keep.Add(ship.DetectionId);
            }
        });

        _logger.LogInformation("dark-vessel match over {Total} ships: {Breakdown}",
            ships.Count, string.Join(", ", counts.Select(kv => $"{kv.Key}={kv.Value}")));

        return keep;
    }
}
