using BaklavaBackend.Common;
using BaklavaBackend.Services;
using Microsoft.AspNetCore.Mvc;

namespace BaklavaBackend.Controllers;

[ApiController]
[Route("api/scenes")]
public class ScenesController : ControllerBase
{
    private readonly JetsonClientService _client;

    public ScenesController(JetsonClientService client)
    {
        _client = client;
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
        return Content(content, "application/json");
    }

    /// POST /api/scenes/sync — pulls every available JSON from the Jetson
    /// (jetson_client.sh get-all)
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
