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

    public DarkVesselMatchService(HttpClient http, IConfiguration config, ILogger<DarkVesselMatchService> logger)
    {
        _http = http;
        _logger = logger;

        var apiKey = config["DarkVessel:ApiKey"];
        if (string.IsNullOrWhiteSpace(apiKey))
            throw new InvalidOperationException("DarkVessel:ApiKey is not configured");
        _http.DefaultRequestHeaders.Add("X-Api-Key", apiKey);

        _maxConcurrentMatches = int.TryParse(config["DarkVessel:MaxConcurrentMatches"], out var m) ? m : 8;
    }

    public async Task<bool> IsDarkAsync(string detectionId, double lat, double lon, DateTime timestampUtc, double? headingDeg, CancellationToken ct)
    {
        var request = new MatchRequestDto(detectionId, lat, lon, timestampUtc, headingDeg);

        using var response = await _http.PostAsJsonAsync("/api/match", request, JsonOptions, ct);
        if (!response.IsSuccessStatusCode)
        {
            var body = await response.Content.ReadAsStringAsync(ct);
            throw new HttpRequestException($"DarkVessel.Api /api/match returned {(int)response.StatusCode}: {body}");
        }

        var result = await response.Content.ReadFromJsonAsync<MatchResultDto>(JsonOptions, ct)
            ?? throw new HttpRequestException("DarkVessel.Api /api/match returned an empty body");

        return result.Status == "Dark";
    }

    /// <summary>
    /// Matches many ships concurrently, bounded by DarkVessel:MaxConcurrentMatches
    /// (default 8) -- each call fans out to two further HTTP calls inside
    /// DarkVessel.Api, so an unbounded Task.WhenAll over a whole scene isn't safe.
    /// Returns the DetectionIds that came back "Dark".
    /// </summary>
    public async Task<HashSet<string>> FilterDarkAsync(IReadOnlyList<ShipDetection> ships, DateTime timestampUtc, CancellationToken ct)
    {
        var dark = new HashSet<string>();
        var gate = new object();

        await Parallel.ForEachAsync(ships, new ParallelOptions { MaxDegreeOfParallelism = _maxConcurrentMatches, CancellationToken = ct }, async (ship, token) =>
        {
            var isDark = await IsDarkAsync(ship.DetectionId, ship.Lat, ship.Lon, timestampUtc, ship.HeadingDeg, token);
            if (isDark)
            {
                lock (gate)
                {
                    dark.Add(ship.DetectionId);
                }
            }
        });

        return dark;
    }
}
