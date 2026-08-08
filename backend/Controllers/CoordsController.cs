using System.Text.Json;
using BaklavaBackend.Common;
using BaklavaBackend.Services;
using Microsoft.AspNetCore.Mvc;

namespace BaklavaBackend.Controllers;

[ApiController]
[Route("api/coords")]
public class CoordsController : ControllerBase
{
    private readonly JetsonClientService _client;

    public CoordsController(JetsonClientService client)
    {
        _client = client;
    }

    /// POST /api/coords?name=optional_name
    /// jetson_client.sh send-coords, which validates and forwards it.
    [HttpPost]
    public async Task<IActionResult> Send([FromQuery] string? name, CancellationToken ct)
    {
        if (name is not null && !Validation.IsSafeName(name))
            return BadRequest(new { error = "invalid name" });

        using var reader = new StreamReader(Request.Body);
        var body = await reader.ReadToEndAsync(ct);

        try
        {
            JsonDocument.Parse(body);
        }
        catch (JsonException)
        {
            return BadRequest(new { error = "body is not valid JSON" });
        }

        var tmpFile = Path.Combine(Path.GetTempPath(), $"coords_{Guid.NewGuid():N}.json");
        await System.IO.File.WriteAllTextAsync(tmpFile, body, ct);

        try
        {
            var args = name is not null
                ? new[] { "send-coords", tmpFile, name }
                : new[] { "send-coords", tmpFile };

            var result = await _client.RunAsync(args, ct: ct);
            if (result.ExitCode != 0)
                return StatusCode(502, new { error = result.StdErr.Trim() });

            return Ok(new { message = result.StdOut.Trim() });
        }
        finally
        {
            System.IO.File.Delete(tmpFile);
        }
    }
}
