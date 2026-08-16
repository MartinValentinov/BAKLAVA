using BaklavaBackend.Common;
using BaklavaBackend.Services;
using DarkVessel.Core;
using DarkVessel.Infrastructure;

var builder = WebApplication.CreateBuilder(args);

builder.Configuration.RequireAll(
    "JetsonClient:ScriptPath",
    "JetsonClient:DestDir");

builder.Services.AddControllers();
builder.Services.AddEndpointsApiExplorer();
builder.Services.AddSwaggerGen();
builder.Services.AddSingleton<JetsonClientService>();
builder.Services.AddSingleton<SceneCatalogService>();

builder.Services.Configure<MongoAisOptions>(builder.Configuration.GetSection(MongoAisOptions.SectionName));
builder.Services.PostConfigure<MongoAisOptions>(options =>
{
    if (string.IsNullOrWhiteSpace(options.ConnectionString))
        options.ConnectionString = Environment.GetEnvironmentVariable("MONGODB_URI") ?? "";
});

var mongoConnectionString = builder.Configuration["Mongo:ConnectionString"]
    ?? Environment.GetEnvironmentVariable("MONGODB_URI");
if (!string.IsNullOrWhiteSpace(mongoConnectionString))
    builder.Services.AddSingleton<IAisSource, MongoAisSource>();

builder.Services.AddSingleton(sp => new DarkVesselMatchService(
    sp.GetService<IAisSource>(),
    sp.GetRequiredService<IConfiguration>(),
    sp.GetRequiredService<ILogger<DarkVesselMatchService>>()));

var allowedOrigins = builder.Configuration.GetSection("Cors:AllowedOrigins").Get<string[]>() ?? Array.Empty<string>();
builder.Services.AddCors(options =>
{
    options.AddDefaultPolicy(policy =>
    {
        policy.WithOrigins(allowedOrigins)
              .AllowAnyHeader()
              .AllowAnyMethod();
    });
});

var app = builder.Build();

if (app.Environment.IsDevelopment())
{
    app.UseDeveloperExceptionPage();
    app.UseSwagger();
    app.UseSwaggerUI();
}

app.UseCors();
app.UseAuthorization();
app.MapControllers();

app.Run();
