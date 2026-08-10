using System.Diagnostics;
using System.Runtime.InteropServices;
using BaklavaBackend.Common;

namespace BaklavaBackend.Services;

public record ProcessResult(int ExitCode, string StdOut, string StdErr);

public class JetsonClientService
{
    private readonly string _scriptPath;
    private readonly int _timeoutSeconds;
    private readonly int _processTimeoutSeconds;
    private readonly string? _token;
    private readonly string? _backend;
    private readonly ILogger<JetsonClientService> _logger;

    public string DestDir { get; }

    public JetsonClientService(IConfiguration config, ILogger<JetsonClientService> logger)
    {
        _logger = logger;
        _scriptPath = config.Require("JetsonClient:ScriptPath");
        DestDir = config.Require("JetsonClient:DestDir");
        _timeoutSeconds = int.TryParse(config["JetsonClient:TimeoutSeconds"], out var t) ? t : 60;
        _processTimeoutSeconds = int.TryParse(config["JetsonClient:ProcessTimeoutSeconds"], out var pt) ? pt : 1800;
        _token = config["JetsonClient:Token"];
        _backend = config["JetsonClient:Backend"];

        if (!File.Exists(_scriptPath))
            _logger.LogWarning("jetson_client.sh not found at {Path}", _scriptPath);

        if (string.IsNullOrWhiteSpace(_token) &&
            string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("BAKLAVA_TOKEN")))
        {
            _logger.LogWarning(
                "No JetsonClient:Token configured and no BAKLAVA_TOKEN in the environment; "
                + "the listener daemon will answer 401 unless it is running unauthenticated");
        }
    }

    public Task<ProcessResult> RunProcessAsync(IEnumerable<string> args, CancellationToken ct = default) =>
        RunAsync(args, TimeSpan.FromSeconds(_processTimeoutSeconds), ct: ct);

    public async Task<ProcessResult> RunAsync(IEnumerable<string> args, TimeSpan? timeout = null, string? stdin = null, CancellationToken ct = default)
    {
        var psi = new ProcessStartInfo
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = stdin is not null,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            psi.FileName = "bash";
            psi.ArgumentList.Add(_scriptPath);
        }
        else
        {
            psi.FileName = _scriptPath;
        }
        foreach (var a in args)
            psi.ArgumentList.Add(a);

        psi.EnvironmentVariables["BAKLAVA_DEST_DIR"] = DestDir.Replace('\\', '/');

        if (!string.IsNullOrWhiteSpace(_token))
            psi.EnvironmentVariables["BAKLAVA_TOKEN"] = _token;

        if (!string.IsNullOrWhiteSpace(_backend))
            psi.EnvironmentVariables["BAKLAVA_BACKEND"] = _backend;

        using var process = new Process { StartInfo = psi };

        var effectiveTimeout = timeout ?? TimeSpan.FromSeconds(_timeoutSeconds);
        using var timeoutCts = new CancellationTokenSource(effectiveTimeout);
        using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(ct, timeoutCts.Token);

        _logger.LogInformation("Running: {Script} {Args}", _scriptPath, string.Join(' ', args));
        process.Start();

        if (stdin is not null)
        {
            await process.StandardInput.WriteAsync(stdin);
            process.StandardInput.Close();
        }

        var stdOutTask = process.StandardOutput.ReadToEndAsync();
        var stdErrTask = process.StandardError.ReadToEndAsync();

        try
        {
            await process.WaitForExitAsync(linkedCts.Token);
        }
        catch (OperationCanceledException) when (timeoutCts.IsCancellationRequested)
        {
            TryKill(process);
            return new ProcessResult(-1, "", $"timed out after {effectiveTimeout.TotalSeconds}s");
        }

        var stdOut = await stdOutTask;
        var stdErr = await stdErrTask;

        if (process.ExitCode != 0)
            _logger.LogWarning("jetson_client.sh exited {Code}: {Err}", process.ExitCode, stdErr.Trim());

        return new ProcessResult(process.ExitCode, stdOut, stdErr);
    }

    private static void TryKill(Process process)
    {
        try
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
        }
        catch
        {
        }
    }
}
