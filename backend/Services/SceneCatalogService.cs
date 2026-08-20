using System.Collections.Concurrent;
using System.Text.Json.Nodes;
using BaklavaBackend.Common;

namespace BaklavaBackend.Services;

public class SceneCatalogService
{
    private readonly JetsonClientService _client;
    private readonly ILogger<SceneCatalogService> _logger;

    private readonly ConcurrentDictionary<string, CachedScene> _scenes = new();
    private readonly ConcurrentDictionary<string, SemaphoreSlim> _locks = new();

    public SceneCatalogService(JetsonClientService client, ILogger<SceneCatalogService> logger)
    {
        _client = client;
        _logger = logger;
    }

    public record CachedScene(JsonObject Normalized, JsonObject? Raw);

    public string DestDir => _client.DestDir;

    public void Forget(string name)
    {
        _scenes.TryRemove(name, out _);
        TryDelete(OverviewPath(name));
    }

    public async Task<IReadOnlyList<string>> ListNamesAsync(CancellationToken ct)
    {
        var result = await _client.RunAsync(new[] { "list" }, ct: ct);
        if (result.ExitCode != 0)
            throw new JetsonException(result.StdErr.Trim());

        return result.StdOut
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .ToArray();
    }

    public async Task<CachedScene?> GetSceneAsync(string name, CancellationToken ct)
    {
        if (_scenes.TryGetValue(name, out var hit))
            return hit;

        var gate = _locks.GetOrAdd(name, _ => new SemaphoreSlim(1, 1));
        await gate.WaitAsync(ct);
        try
        {
            if (_scenes.TryGetValue(name, out hit))
                return hit;

            var result = await _client.RunAsync(new[] { "get", name }, ct: ct);
            if (result.ExitCode != 0)
            {
                _logger.LogWarning("scene {Scene} could not be fetched: {Err}", name, result.StdErr.Trim());
                return null;
            }

            var path = Path.Combine(_client.DestDir, name + ".json");
            if (!File.Exists(path))
            {
                _logger.LogWarning("jetson_client.sh get {Scene} succeeded but wrote no file", name);
                return null;
            }

            JsonObject raw;
            try
            {
                raw = JsonNode.Parse(await File.ReadAllTextAsync(path, ct))!.AsObject();
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "scene {Scene} is not valid JSON", name);
                return null;
            }

            var isCpp = SceneNormalizer.IsCpp(raw);
            var cached = new CachedScene(SceneNormalizer.Normalize(raw), isCpp ? raw : null);
            _scenes[name] = cached;
            return cached;
        }
        finally
        {
            gate.Release();
        }
    }

    public string OverviewPath(string name) =>
        Path.Combine(_client.DestDir, name + ".overview.jpg");

    public async Task<string?> EnsureOverviewAsync(string name, CancellationToken ct)
    {
        var path = OverviewPath(name);
        if (File.Exists(path) && new FileInfo(path).Length > 0)
            return path;

        var result = await _client.RunAsync(new[] { "overview", name }, ct: ct);
        if (result.ExitCode != 0 || !File.Exists(path))
        {
            _logger.LogInformation("scene {Scene} has no overview: {Err}", name, result.StdErr.Trim());
            return null;
        }
        return path;
    }

    private void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); }
        catch (Exception ex) { _logger.LogWarning(ex, "could not delete {Path}", path); }
    }
}

public class JetsonException : Exception
{
    public JetsonException(string message) : base(message) { }
}
