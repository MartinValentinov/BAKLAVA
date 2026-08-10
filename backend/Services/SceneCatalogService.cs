using System.Collections.Concurrent;
using System.Text.Json.Nodes;
using BaklavaBackend.Common;

namespace BaklavaBackend.Services;

/// Everything the map frontend needs out of the Jetson, cached.
///
/// Why a cache at all: the scene list has to carry each scene's footprint, and
/// a footprint only exists inside that scene's own JSON. Building the list
/// therefore means reading every scene, and every read is a jetson_client.sh
/// process over the link. Doing that on each page load would make picking a
/// scene take seconds and would hammer a link that is the scarce resource here.
///
/// The cache is keyed on the scene name and never invalidated by time: a
/// processed scene is immutable - the detector writes it once, under a name
/// that includes the acquisition timestamp. Reprocessing produces a new name.
/// POST /api/scenes/{name}/process clears the entry anyway, so a re-run of the
/// same name is picked up.
public class SceneCatalogService
{
    private readonly JetsonClientService _client;
    private readonly ILogger<SceneCatalogService> _logger;

    private readonly ConcurrentDictionary<string, CachedScene> _scenes = new();
    // One fetch per scene even when several requests want it at once: the list
    // endpoint fans out over every scene, and without this a cold cache would
    // start N identical downloads.
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
        // The downloaded artefacts are stale too once a scene is reprocessed.
        TryDelete(OverviewPath(name));
        var crops = CropsDir(name);
        if (Directory.Exists(crops))
        {
            try { Directory.Delete(crops, recursive: true); }
            catch (Exception ex) { _logger.LogWarning(ex, "could not clear {Dir}", crops); }
        }
    }

    /// The processed scene names on the Jetson, newest first.
    public async Task<IReadOnlyList<string>> ListNamesAsync(CancellationToken ct)
    {
        var result = await _client.RunAsync(new[] { "list" }, ct: ct);
        if (result.ExitCode != 0)
            throw new JetsonException(result.StdErr.Trim());

        return result.StdOut
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .ToArray();
    }

    /// One scene's JSON, normalised onto meta+ships. Null when the Jetson does
    /// not have it.
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

            // Keep the pre-normalisation object too: footprint_lonlat lives on
            // the cpp shape and SceneNormalizer does not carry it across.
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

    // ---------------------------------------------------------------- overview

    public string OverviewPath(string name) =>
        Path.Combine(_client.DestDir, name + ".overview.jpg");

    /// The decimated whole-scene render, downloading it on first use. Null when
    /// the scene has none - the legacy Python backend produces no overview.
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

    // ------------------------------------------------------------------ crops

    public string CropsDir(string name) => Path.Combine(_client.DestDir, name + "-crops");

    public string CropsManifestPath(string name) => Path.Combine(CropsDir(name), "manifest.json");

    /// The crop manifest, downloading the thumbnail tier on first use.
    ///
    /// Thumbnails, not full-resolution: the whole point of the tiering is that
    /// the ground pulls ~2.5 MB to look at and only then asks for the crops it
    /// actually wants. Fetching the full set here would defeat it.
    public async Task<JsonObject?> EnsureCropsAsync(string name, bool full, CancellationToken ct)
    {
        var args = full
            ? new[] { "crops", name, "--full" }
            : new[] { "crops", name };

        var manifest = CropsManifestPath(name);
        if (!full && File.Exists(manifest))
            return await ReadManifestAsync(manifest, ct);

        // Long-running: a full tier is tens of megabytes over the link, and the
        // client is incremental, so a re-run only costs what is missing.
        var result = await _client.RunProcessAsync(args, ct);
        if (!File.Exists(manifest))
        {
            _logger.LogInformation("scene {Scene} has no crops: {Err}", name, result.StdErr.Trim());
            return null;
        }
        if (result.ExitCode != 0)
            _logger.LogWarning("crops {Scene} finished with errors: {Err}", name, result.StdErr.Trim());

        return await ReadManifestAsync(manifest, ct);
    }

    private async Task<JsonObject?> ReadManifestAsync(string path, CancellationToken ct)
    {
        try
        {
            return JsonNode.Parse(await File.ReadAllTextAsync(path, ct))!.AsObject();
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "crop manifest {Path} is not valid JSON", path);
            return null;
        }
    }

    /// Resolve a manifest-relative crop path to a file on disk, refusing
    /// anything that escapes the scene's own directory.
    public string? ResolveCropFile(string name, string relative)
    {
        var root = Path.GetFullPath(CropsDir(name));
        var full = Path.GetFullPath(Path.Combine(root, relative));
        if (!full.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.Ordinal))
            return null;
        return File.Exists(full) ? full : null;
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
