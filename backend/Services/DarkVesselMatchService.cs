using System.Text.Json;
using System.Text.Json.Serialization;

namespace BaklavaBackend.Services;

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
