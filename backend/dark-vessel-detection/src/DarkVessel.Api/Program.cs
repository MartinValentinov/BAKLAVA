using DarkVessel.Core;
using DarkVessel.Infrastructure;

using System.Text.Json.Serialization;

var builder = WebApplication.CreateBuilder(args);

builder.Services.ConfigureHttpJsonOptions(options =>
{
    options.SerializerOptions.Converters.Add(new JsonStringEnumConverter());
});

builder.Services.Configure<AisStreamOptions>(builder.Configuration.GetSection(AisStreamOptions.SectionName));
builder.Services.Configure<BaklavaApiOptions>(builder.Configuration.GetSection(BaklavaApiOptions.SectionName));
builder.Services.Configure<MongoAisOptions>(builder.Configuration.GetSection(MongoAisOptions.SectionName));
builder.Services.PostConfigure<MongoAisOptions>(options =>
{
    if (string.IsNullOrWhiteSpace(options.ConnectionString))
    {
        options.ConnectionString = Environment.GetEnvironmentVariable("MONGODB_URI") ?? "";
    }
});

var archiveBackend = (builder.Configuration["Ais:Source"] ?? "mongo").ToLowerInvariant();
switch (archiveBackend)
{
    case "mongo":
        builder.Services.AddSingleton<IAisArchive, MongoAisSource>();
        break;

    case "http":
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
        builder.Services.AddSingleton<IAisArchive>(sp => sp.GetRequiredService<HttpAisSource>());
        break;

    case "mysql":
        var connectionString = builder.Configuration.GetConnectionString("AisArchive");
        if (string.IsNullOrWhiteSpace(connectionString))
        {
            throw new InvalidOperationException(
                "Ais:Source is \"mysql\" but ConnectionStrings:AisArchive is not configured. Set it with " +
                "`dotnet user-secrets set \"ConnectionStrings:AisArchive\" \"...\"`.");
        }
        builder.Services.AddSingleton<IAisArchive>(_ => new AisStore(connectionString));
        break;

    default:
        throw new InvalidOperationException(
            $"Ais:Source \"{archiveBackend}\" is not a known archive backend; use \"mongo\", \"http\" or \"mysql\".");
}

builder.Services.AddSingleton<CollectorState>();
builder.Services.AddHostedService<AisStreamCollectorService>();

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
            policy.AllowAnyOrigin().AllowAnyHeader().AllowAnyMethod();
        }
    });
});

var app = builder.Build();

app.UseDefaultFiles();
app.UseStaticFiles();

app.UseCors("Frontend");

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

app.MapGet("/api/archive-stats", async (IAisArchive store, CancellationToken ct) =>
{
    var stats = await store.ArchiveStatsAsync(ct);
    return Results.Ok(stats);
});

app.MapPost("/api/match", async (MatchRequest request, IAisArchive store, CancellationToken ct) =>
{
    var timestampUtc = DateTime.SpecifyKind(request.TimestampUtc, DateTimeKind.Utc);
    var detection = new Detection(request.DetectionId, request.Lat, request.Lon, timestampUtc, request.HeadingDeg);
    var result = await Matcher.MatchDetectionAsync(detection, store, ct: ct).ConfigureAwait(false);
    return Results.Ok(result);
});

app.Run();

public sealed record MatchRequest(string DetectionId, double Lat, double Lon, DateTime TimestampUtc, double? HeadingDeg = null);