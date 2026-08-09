using System.Globalization;
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
    private readonly DarkVesselMatchService _darkVessel;

    public ScenesController(JetsonClientService client, DarkVesselMatchService darkVessel)
    {
        _client = client;
        _darkVessel = darkVessel;
    }

    /// GET /api/scenes — list available processed scene names, newest first.
    [HttpGet]
    public async Task<IActionResult> List(CancellationToken ct)
    {
        var result = await _client.RunAsync(new[] { "list" }, ct: ct);
        if (result.ExitCode != 0)
            return StatusCode(502, new { error = result.StdErr.Trim() });

        var names = result.StdOut
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        return Ok(names);
    }

    /// GET /api/scenes/{name} — fetch one scene's JSON by exact name (no .json suffix).
    [HttpGet("{name}")]
    public async Task<IActionResult> Get(string name, CancellationToken ct)
    {
        if (!Validation.IsSafeName(name))
            return BadRequest(new { error = "invalid scene name" });

        var result = await _client.RunAsync(new[] { "get", name }, ct: ct);
        if (result.ExitCode != 0)
            return NotFound(new { error = result.StdErr.Trim() });

        var path = Path.Combine(_client.DestDir, name + ".json");
        if (!System.IO.File.Exists(path))
            return StatusCode(502, new { error = "jetson_client.sh reported success but wrote no file" });

        var content = await System.IO.File.ReadAllTextAsync(path, ct);

        JsonObject root;
        try
        {
            root = JsonNode.Parse(content)!.AsObject();
        }
        catch (Exception ex)
        {
            return StatusCode(502, new { error = $"scene JSON could not be parsed: {ex.Message}" });
        }

        var meta = root["meta"]?.AsObject();
        var ships = root["ships"]?.AsArray();
        if (meta is null || ships is null)
            return Content(content, "application/json");

        var acquiredRaw = meta["acquired"]?.GetValue<string>();
        if (string.IsNullOrEmpty(acquiredRaw) ||
            !DateTime.TryParse(acquiredRaw, CultureInfo.InvariantCulture,
                DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out var acquiredUtc))
        {
            return StatusCode(502, new { error = $"scene meta.acquired could not be parsed as a timestamp: '{acquiredRaw}'" });
        }

        var detections = new List<ShipDetection>();
        foreach (var shipNode in ships)
        {
            var ship = shipNode!.AsObject();
            var id = ship["id"]!.GetValue<int>();
            var lat = ship["latitude"]!.GetValue<double>();
            var lon = ship["longitude"]!.GetValue<double>();
            var heading = ship["heading"]?.GetValue<double?>();
            detections.Add(new ShipDetection($"{name}-{id}", lat, lon, heading));
        }

        HashSet<string> darkDetectionIds;
        try
        {
            darkDetectionIds = await _darkVessel.FilterDarkAsync(detections, acquiredUtc, ct);
        }
        catch (Exception ex)
        {
            return StatusCode(502, new { error = $"dark-vessel match failed: {ex.Message}" });
        }

        for (var i = ships.Count - 1; i >= 0; i--)
        {
            var id = ships[i]!.AsObject()["id"]!.GetValue<int>();
            if (!darkDetectionIds.Contains($"{name}-{id}"))
                ships.RemoveAt(i);
        }
        meta["ships"] = ships.Count;

        return Content(root.ToJsonString(), "application/json");
    }

    /// POST /api/scenes/sync — pulls every available JSON from the Jetson
    /// (jetson_client.sh get-all). Returns every ship as-is, unlike GET
    /// /api/scenes/{name} above -- would need the same dark-vessel filtering
    /// applied if the frontend ever starts using this endpoint.
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

    /// POST /api/scenes/available — list raw scene images on the Jetson that
    /// haven't been processed yet (jetson_client.sh list-images)
    [HttpPost("available")]
    public async Task<IActionResult> Available(CancellationToken ct)
    {
        var result = await _client.RunAsync(new[] { "list-images" }, ct: ct);
        if (result.ExitCode != 0)
            return StatusCode(502, new { error = result.StdErr.Trim() });

        var names = result.StdOut
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        return Ok(names);
    }

    /// POST /api/scenes/{name}/process — run detection for one raw image on
    /// the Jetson (jetson_client.sh process NAME). Long-running.
    [HttpPost("{name}/process")]
    public async Task<IActionResult> Process(string name, CancellationToken ct)
    {
        if (!Validation.IsSafeName(name))
            return BadRequest(new { ok = false, error = "invalid image name" });

        var result = await _client.RunProcessAsync(new[] { "process", name }, ct);
        if (result.ExitCode != 0)
            return StatusCode(502, new { ok = false, error = result.StdErr.Trim() });

        return Ok(new { ok = true, message = result.StdOut.Trim() });
    }
}
