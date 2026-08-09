using DarkVessel.Core;
using DarkVessel.Infrastructure;

using System.Text.Json.Serialization;

var builder = WebApplication.CreateBuilder(args);

// Without this, MatchStatus (Matched/Dark/UnknownNoCoverage) serializes as a
// bare integer (0/1/2) instead of its name -- technically correct, but not
// what API_REFERENCE.md documents and not something a UI developer should
// have to reverse-engineer.
builder.Services.ConfigureHttpJsonOptions(options =>
{
    options.SerializerOptions.Converters.Add(new JsonStringEnumConverter());
});

builder.Services.Configure<AisStreamOptions>(builder.Configuration.GetSection(AisStreamOptions.SectionName));
builder.Services.Configure<BaklavaApiOptions>(builder.Configuration.GetSection(BaklavaApiOptions.SectionName));

// HttpAisSource talks to the Baklava HTTP API (supm.online) instead of MySQL
// directly -- MySQL's own port isn't reachable from outside that host, but
// this API is (plain HTTPS, already proven working). AddHttpClient<T> both
// registers HttpAisSource for DI (used below, and by AisStreamCollectorService)
// and gives it a properly pooled HttpClient with BaseAddress pre-set, so every
// call in HttpAisSource can just use a relative "?resource=..." URL.
builder.Services.AddHttpClient<HttpAisSource>((sp, client) =>
{
    var options = sp.GetRequiredService<Microsoft.Extensions.Options.IOptions<BaklavaApiOptions>>().Value;
    if (string.IsNullOrWhiteSpace(options.ApiKey))
    {
        throw new InvalidOperationException(
            "Baklava:ApiKey is not configured. Set it with " +
            "`dotnet user-secrets set \"Baklava:ApiKey\" \"...\"` -- never in appsettings.json, which is committed to git.");
    }
    client.BaseAddress = new Uri(options.BaseUrl);
});

builder.Services.AddSingleton<CollectorState>();
builder.Services.AddHostedService<AisStreamCollectorService>();

// Lets a separately-built UI (different port, different domain, different
// framework entirely -- doesn't matter, it's just an HTTP client) call this
// API from browser JavaScript. Without this, the browser silently blocks
// every fetch() from a different origin, no matter how correct the request is.
var allowedOrigins = builder.Configuration.GetSection("Cors:AllowedOrigins").Get<string[]>() ?? [];
builder.Services.AddCors(options =>
{
    options.AddPolicy("Frontend", policy =>
    {
        if (allowedOrigins.Length > 0)
        {
            policy.WithOrigins(allowedOrigins).AllowAnyHeader().AllowAnyMethod();
        }
        else
        {
            // Dev default: wide open, so this works immediately no matter what
            // port/tool the UI dev is running. Set Cors:AllowedOrigins to a
            // specific list (e.g. ["https://your-real-ui-domain"]) before this
            // is reachable by anyone but a trusted developer.
            policy.AllowAnyOrigin().AllowAnyHeader().AllowAnyMethod();
        }
    });
});

var app = builder.Build();

app.UseDefaultFiles();
app.UseStaticFiles();

// Must come before the API-key middleware below: browsers send an unauthenticated
// CORS "preflight" (OPTIONS) request before the real one, and UseCors answers
// that itself without forwarding it downstream. If it ran after the API-key
// check, every preflight would get rejected as unauthorized and the browser
// would never even attempt the real request.
app.UseCors("Frontend");

// Simple shared-secret gate for /api/*. Not a port of auth.php's session+role+CSRF
// system -- that was tied to the PHP site's own login. This is a placeholder
// suitable for a single trusted operator; swap for real auth (ASP.NET Identity,
// JWT, whatever the rest of the ground system ends up using) before this is
// reachable from anywhere but your own machine.
var apiKey = builder.Configuration["Api:ApiKey"];
app.Use(async (context, next) =>
{
    if (context.Request.Path.StartsWithSegments("/api"))
    {
        if (string.IsNullOrEmpty(apiKey))
        {
            context.Response.StatusCode = StatusCodes.Status500InternalServerError;
            await context.Response.WriteAsJsonAsync(new { ok = false, error = "Api:ApiKey is not configured on the server" });
            return;
        }
        if (context.Request.Headers["X-Api-Key"] != apiKey)
        {
            context.Response.StatusCode = StatusCodes.Status401Unauthorized;
            await context.Response.WriteAsJsonAsync(new { ok = false, error = "missing or invalid X-Api-Key" });
            return;
        }
    }
    await next();
});

// -- status / control: the .NET equivalent of status.php / control.php -----

app.MapGet("/api/status", (CollectorState state) =>
{
    var snap = state.Snapshot();
    return Results.Ok(new
    {
        ok = true,
        running = snap.Connected,
        desired = snap.Desired.ToString().ToLowerInvariant(),
        coverageId = snap.CoverageId,
        messages = snap.Messages,
        rowsWritten = snap.RowsWritten,
        startedAt = snap.StartedAt,
        lastMessageAt = snap.LastMessageAt,
        lastError = snap.LastError,
    });
});

app.MapPost("/api/control/start", (CollectorState state) =>
{
    state.RequestStart();
    return Results.Ok(new { ok = true, desired = "running" });
});

app.MapPost("/api/control/stop", (CollectorState state) =>
{
    state.RequestStop();
    return Results.Ok(new { ok = true, desired = "stopped" });
});

app.MapGet("/api/archive-stats", async (HttpAisSource store, CancellationToken ct) =>
{
    var stats = await store.ArchiveStatsAsync(ct);
    return Results.Ok(stats);
});

// -- matching: the new thing this .NET backend adds -------------------------
// Wraps DarkVessel.Core.Matcher (the direct port of intelligence/matching.py)
// over HTTP, backed by the real database via HttpAisSource -> the Baklava API.

app.MapPost("/api/match", async (MatchRequest request, HttpAisSource store, CancellationToken ct) =>
{
    var timestampUtc = DateTime.SpecifyKind(request.TimestampUtc, DateTimeKind.Utc);
    var detection = new Detection(request.DetectionId, request.Lat, request.Lon, timestampUtc, request.HeadingDeg);
    var result = await Matcher.MatchDetectionAsync(detection, store, ct: ct).ConfigureAwait(false);
    return Results.Ok(result);
});

app.Run();

public sealed record MatchRequest(string DetectionId, double Lat, double Lon, DateTime TimestampUtc, double? HeadingDeg = null);