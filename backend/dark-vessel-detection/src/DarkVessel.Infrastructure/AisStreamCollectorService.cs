using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using DarkVessel.Core;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;

namespace DarkVessel.Infrastructure;

public sealed class AisStreamCollectorService : BackgroundService
{
    private readonly HttpAisSource _store;
    private readonly AisStreamOptions _options;
    private readonly CollectorState _state;
    private readonly ILogger<AisStreamCollectorService> _logger;

    public AisStreamCollectorService(
        HttpAisSource store, IOptions<AisStreamOptions> options, CollectorState state, ILogger<AisStreamCollectorService> logger)
    {
        _store = store;
        _options = options.Value;
        _state = state;
        _logger = logger;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            if (_state.Snapshot().Desired != DesiredState.Running)
            {
                await Task.Delay(TimeSpan.FromSeconds(1), stoppingToken).ConfigureAwait(false);
                continue;
            }

            if (string.IsNullOrWhiteSpace(_options.ApiKey))
            {
                _state.OnError("AisStream:ApiKey is not set -- configure it via user-secrets or an environment variable, never in a committed file.");
                await Task.Delay(TimeSpan.FromSeconds(5), stoppingToken).ConfigureAwait(false);
                continue;
            }

            long? coverageId = null;
            try
            {
                coverageId = await _store.OpenCoverageAsync(
                    JsonSerializer.Serialize(_options.BoundingBoxes), Environment.MachineName, stoppingToken)
                    .ConfigureAwait(false);
                _state.OnSessionOpened(coverageId.Value);
                _logger.LogInformation("Coverage #{CoverageId} opened", coverageId);

                await RunOneSessionAsync(coverageId.Value, stoppingToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Collector session failed, reconnecting in 5s");
                _state.OnError(ex.Message);
            }
            finally
            {
                if (coverageId is not null)
                {
                    try
                    {
                        await _store.CloseCoverageAsync(coverageId.Value, 0, CancellationToken.None).ConfigureAwait(false);
                        _logger.LogInformation("Coverage #{CoverageId} closed", coverageId);
                    }
                    catch (Exception ex)
                    {
                        _logger.LogError(ex, "Could not close coverage #{CoverageId}", coverageId);
                    }
                    _state.OnSessionClosed();
                }
            }

            if (!stoppingToken.IsCancellationRequested && _state.Snapshot().Desired == DesiredState.Running)
            {
                await Task.Delay(TimeSpan.FromSeconds(5), stoppingToken).ConfigureAwait(false);
            }
        }
    }

    private async Task RunOneSessionAsync(long coverageId, CancellationToken ct)
    {
        using var socket = new ClientWebSocket();
        await socket.ConnectAsync(new Uri("wss://stream.aisstream.io/v0/stream"), ct).ConfigureAwait(false);

        var subscription = new AisStreamSubscription
        {
            ApiKey = _options.ApiKey,
            BoundingBoxes = _options.BoundingBoxes,
            FilterMessageTypes = _options.MessageTypes,
        };
        await SendJsonAsync(socket, subscription, ct).ConfigureAwait(false);
        _logger.LogInformation("Subscribed to {Count} bounding boxes", _options.BoundingBoxes.Length);

        var positions = new Dictionary<(long Mmsi, DateTime Minute), PositionWrite>();
        var vessels = new Dictionary<long, VesselWrite>();
        int messageCount = 0;
        var lastFlush = DateTime.UtcNow;
        var lastMessageAt = DateTime.UtcNow;
        var idleLimit = TimeSpan.FromSeconds(_options.IdleReconnectSeconds);

        var buffer = new byte[64 * 1024];

        while (socket.State == WebSocketState.Open && !ct.IsCancellationRequested
               && _state.Snapshot().Desired == DesiredState.Running)
        {
            using var receiveCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            receiveCts.CancelAfter(TimeSpan.FromSeconds(5));

            string? json;
            try
            {
                json = await ReceiveTextMessageAsync(socket, buffer, receiveCts.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (!ct.IsCancellationRequested)
            {
                json = null;
            }

            if (json is not null)
            {
                messageCount++;
                lastMessageAt = DateTime.UtcNow;
                HandleMessage(json, positions, vessels);
            }

            if (DateTime.UtcNow - lastMessageAt > idleLimit)
            {
                _logger.LogWarning("No messages for {Seconds}s, forcing reconnect", idleLimit.TotalSeconds);
                break;
            }

            if (DateTime.UtcNow - lastFlush >= TimeSpan.FromSeconds(_options.FlushSeconds))
            {
                await FlushAsync(coverageId, positions, vessels, messageCount, lastMessageAt, ct).ConfigureAwait(false);
                lastFlush = DateTime.UtcNow;
            }
        }

        await FlushAsync(coverageId, positions, vessels, messageCount, lastMessageAt, CancellationToken.None).ConfigureAwait(false);

        if (socket.State == WebSocketState.Open)
        {
            await socket.CloseAsync(WebSocketCloseStatus.NormalClosure, "shutting down", CancellationToken.None).ConfigureAwait(false);
        }
    }

    private async Task FlushAsync(
        long coverageId,
        Dictionary<(long Mmsi, DateTime Minute), PositionWrite> positions,
        Dictionary<long, VesselWrite> vessels,
        int messageCount,
        DateTime lastMessageAt,
        CancellationToken ct)
    {
        int written = 0;
        if (positions.Count > 0)
        {
            written = await _store.InsertPositionsAsync(positions.Values.ToList(), ct).ConfigureAwait(false);
            _logger.LogInformation("+{Written} rows ({Buffered} buffered this flush)", written, positions.Count);
            positions.Clear();
        }
        if (vessels.Count > 0)
        {
            await _store.UpsertVesselsAsync(vessels.Values.ToList(), ct).ConfigureAwait(false);
            vessels.Clear();
        }
        await _store.TouchCoverageAsync(coverageId, messageCount, lastMessageAt, ct).ConfigureAwait(false);
        _state.OnFlush(messageCount, written, lastMessageAt);
    }

    private static void HandleMessage(
        string json,
        Dictionary<(long Mmsi, DateTime Minute), PositionWrite> positions,
        Dictionary<long, VesselWrite> vessels)
    {
        AisStreamEnvelope? envelope;
        try
        {
            envelope = JsonSerializer.Deserialize<AisStreamEnvelope>(json);
        }
        catch (JsonException)
        {
            return;
        }
        if (envelope?.MetaData is null)
        {
            return;
        }

        long mmsi = envelope.MetaData.Mmsi;
        var now = DateTime.UtcNow;

        if (envelope.MessageType == "PositionReport" && envelope.Message?.PositionReport is { } report)
        {
            double? sog = report.Sog is >= 102.3 ? null : report.Sog;
            double? cog = report.Cog is >= 360 ? null : report.Cog;
            double? heading = report.TrueHeading is 511 ? null : report.TrueHeading;

            var minute = new DateTime(now.Year, now.Month, now.Day, now.Hour, now.Minute, 0, DateTimeKind.Utc);
            positions[(mmsi, minute)] = new PositionWrite(
                mmsi, minute, report.Latitude, report.Longitude, sog, cog, heading, report.NavigationalStatus);
        }
        else if (envelope.MessageType == "ShipStaticData" && envelope.Message?.ShipStaticData is { } staticData)
        {
            double? lengthM = staticData.Dimension is { A: not null, B: not null } d ? d.A + d.B : null;
            double? widthM = staticData.Dimension is { C: not null, D: not null } d2 ? d2.C + d2.D : null;
            vessels[mmsi] = new VesselWrite(
                mmsi, staticData.Name?.Trim(), staticData.CallSign?.Trim(), staticData.ImoNumber,
                staticData.Type, lengthM, widthM, now);
        }
    }

    private static async Task SendJsonAsync<T>(ClientWebSocket socket, T payload, CancellationToken ct)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(payload);
        await socket.SendAsync(bytes, WebSocketMessageType.Text, endOfMessage: true, ct).ConfigureAwait(false);
    }

    private static async Task<string?> ReceiveTextMessageAsync(ClientWebSocket socket, byte[] buffer, CancellationToken ct)
    {
        using var stream = new MemoryStream();
        WebSocketReceiveResult result;
        do
        {
            result = await socket.ReceiveAsync(buffer, ct).ConfigureAwait(false);
            if (result.MessageType == WebSocketMessageType.Close)
            {
                return null;
            }
            stream.Write(buffer, 0, result.Count);
        } while (!result.EndOfMessage);

        return Encoding.UTF8.GetString(stream.ToArray());
    }
}
