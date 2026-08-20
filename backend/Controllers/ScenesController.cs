using System.Text.Json.Nodes;
using BaklavaBackend.Common;
using BaklavaBackend.Services;
using Microsoft.AspNetCore.Mvc;

namespace BaklavaBackend.Controllers;

[ApiController]
[Route("api/scenes")]
public class ScenesController : ControllerBase
{
    private readonly JetsonClientService _client;
    private readonly SceneCatalogService _catalog;
    private readonly DarkVesselMatchService _darkVessel;
    private readonly ILogger<ScenesController> _logger;

    public ScenesController(JetsonClientService client, SceneCatalogService catalog,
                            DarkVesselMatchService darkVessel, ILogger<ScenesController> logger)
    {
        _client = client;
        _catalog = catalog;
        _darkVessel = darkVessel;
        _logger = logger;
    }

    [HttpGet]
    public async Task<IActionResult> List(CancellationToken ct)
    {
        IReadOnlyList<string> names;
        try
        {
            names = await _catalog.ListNamesAsync(ct);
        }
        catch (JetsonException ex)
        {
            return StatusCode(502, new { error = ex.Message });
        }

        var scenes = new List<SceneSummary>();
        foreach (var name in names)
        {
            if (!Validation.IsSafeName(name))
                continue;

            var cached = await _catalog.GetSceneAsync(name, ct);
            if (cached is null)
                continue;

            var corners = SceneProjector.Footprint(cached.Normalized, cached.Raw);
            if (corners is null)
            {
                _logger.LogInformation(
                    "scene {Scene} has neither a footprint nor a positioned detection; "
                    + "leaving it out of the picker", name);
                continue;
            }

            var hasSar = System.IO.File.Exists(_catalog.OverviewPath(name))
                         || cached.Raw is not null;

            scenes.Add(new SceneSummary(
                Id: name,
                Label: SceneProjector.Label(name, cached.Normalized["meta"] as JsonObject),
                Corners: corners,
                HasSar: hasSar));
        }

        return Ok(new SceneListResponse(scenes));
    }

    [HttpGet("{name}")]
    public async Task<IActionResult> Get(string name, CancellationToken ct)
    {
        if (!Validation.IsSafeName(name))
            return BadRequest(new { error = "invalid scene name" });

        var cached = await _catalog.GetSceneAsync(name, ct);
        if (cached is null)
            return NotFound(new { error = $"no scene '{name}' on the Jetson" });

        var corners = SceneProjector.Footprint(cached.Normalized, cached.Raw);
        if (corners is null)
            return StatusCode(502, new { error = "scene has no footprint and no positioned detections" });

        var darkIds = await ComputeDarkIdsAsync(name, cached.Normalized, ct);
        var vessels = SceneProjector.Vessels(name, cached.Normalized, darkIds);

        SarOverlay? overlay = null;
        var overviewPath = await _catalog.EnsureOverviewAsync(name, ct);
        if (overviewPath is not null)
        {
            var overviewCorners = SceneProjector.OverviewBounds(cached.Raw) ?? corners;
            overlay = new SarOverlay($"/api/scenes/{Uri.EscapeDataString(name)}/overview",
                                     overviewCorners,
                                     SceneProjector.Swath(cached.Raw));
        }

        return Ok(new SceneDetail(
            Id: name,
            Label: SceneProjector.Label(name, cached.Normalized["meta"] as JsonObject),
            Corners: corners,
            Totals: new SceneTotals(vessels.Count, vessels.Count(v => v.Dark)),
            Vessels: vessels,
            SarOverlay: overlay,
            Timings: SceneProjector.Timings(cached.Raw)));
    }

    [HttpGet("{name}/raw")]
    public async Task<IActionResult> Raw(string name, CancellationToken ct)
    {
        if (!Validation.IsSafeName(name))
            return BadRequest(new { error = "invalid scene name" });

        var cached = await _catalog.GetSceneAsync(name, ct);
        if (cached is null)
            return NotFound(new { error = $"no scene '{name}' on the Jetson" });

        return Content(cached.Normalized.ToJsonString(), "application/json");
    }

    [HttpGet("{name}/overview")]
    public async Task<IActionResult> Overview(string name, CancellationToken ct)
    {
        if (!Validation.IsSafeName(name))
            return BadRequest(new { error = "invalid scene name" });

        var path = await _catalog.EnsureOverviewAsync(name, ct);
        if (path is null)
            return NotFound(new { error = $"scene '{name}' has no overview render" });

        return PhysicalFile(path, "image/jpeg", enableRangeProcessing: true);
    }

    [HttpPost("sync")]
    public async Task<IActionResult> Sync(CancellationToken ct)
    {
        var result = await _client.RunAsync(new[] { "get-all" }, ct: ct);
        if (result.ExitCode != 0)
            return StatusCode(502, new { error = result.StdErr.Trim() });

        var scenes = new List<object>();
        foreach (var path in Directory.EnumerateFiles(_client.DestDir, "*.json"))
        {
            var name = Path.GetFileNameWithoutExtension(path);
            var content = await System.IO.File.ReadAllTextAsync(path, ct);
            scenes.Add(new { name, content = System.Text.Json.JsonDocument.Parse(content).RootElement });
        }

        return Ok(scenes);
    }

    [HttpPost("available")]
    public async Task<IActionResult> Available(CancellationToken ct)
    {
        var result = await _client.RunAsync(new[] { "list-images" }, ct: ct);
        if (result.ExitCode != 0)
            return StatusCode(502, new { error = result.StdErr.Trim() });

        var names = result.StdOut
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        var footprints = await AvailableFootprintsAsync(ct);

        return Ok(names.Select(name => new
        {
            name,
            corners = footprints.TryGetValue(name, out var quad) ? quad : null,
        }));
    }

    private async Task<Dictionary<string, JsonNode?>> AvailableFootprintsAsync(CancellationToken ct)
    {
        var none = new Dictionary<string, JsonNode?>();
        try
        {
            var result = await _client.RunAsync(new[] { "image-footprints" }, ct: ct);
            if (result.ExitCode != 0)
            {
                _logger.LogWarning("image-footprints exited {Code}: {Err}",
                                   result.ExitCode, result.StdErr.Trim());
                return none;
            }

            if (JsonNode.Parse(result.StdOut) is not JsonObject parsed)
                return none;

            return parsed.ToDictionary(pair => pair.Key, pair => pair.Value?.DeepClone());
        }
        catch (Exception exc)
        {
            _logger.LogWarning(exc, "could not read image footprints");
            return none;
        }
    }

    [HttpPost("{name}/process")]
    public async Task<IActionResult> Process(string name, CancellationToken ct)
    {
        if (!Validation.IsSafeName(name))
            return BadRequest(new { ok = false, error = "invalid image name" });

        var result = await _client.RunProcessAsync(new[] { "process", name }, ct);
        if (result.ExitCode != 0)
            return StatusCode(502, new { ok = false, error = result.StdErr.Trim() });

        var stem = Path.GetFileNameWithoutExtension(name);
        _catalog.Forget(stem);
        _catalog.Forget($"{stem}__cpp");

        return Ok(new { ok = true, message = result.StdOut.Trim() });
    }

    private async Task<IReadOnlySet<string>?> ComputeDarkIdsAsync(
        string name, JsonObject normalized, CancellationToken ct)
    {
        if (!_darkVessel.Enabled)
            return null;

        var meta = normalized["meta"] as JsonObject;
        var ships = normalized["ships"] as JsonArray;
        if (meta is null || ships is null)
            return null;

        if (!SceneNormalizer.TryGetAcquired(meta, out var acquiredUtc))
        {
            _logger.LogWarning(
                "scene {Scene}: meta.acquired has no usable timestamp ({Raw}); "
                + "reporting every detection as dark", name, meta["acquired"]?.GetValue<string>());
            return null;
        }

        var detections = new List<ShipDetection>();
        foreach (var node in ships)
        {
            if (node is not JsonObject ship) continue;
            var lat = ship["latitude"]?.GetValue<double?>();
            var lon = ship["longitude"]?.GetValue<double?>();
            if (lat is null || lon is null) continue;
            var id = ship["id"]?.GetValue<int>() ?? 0;
            detections.Add(new ShipDetection($"{name}-{id}", lat.Value, lon.Value,
                                             ship["heading"]?.GetValue<double?>()));
        }

        try
        {
            return await _darkVessel.FilterKeepAsync(detections, acquiredUtc, ct);
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "dark-vessel match failed for {Scene}; reporting all as dark", name);
            return null;
        }
    }
}
