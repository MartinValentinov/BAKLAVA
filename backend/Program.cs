using BaklavaBackend.Common;
using BaklavaBackend.Services;

var builder = WebApplication.CreateBuilder(args);

builder.Configuration.RequireAll(
    "JetsonClient:ScriptPath",
    "JetsonClient:DestDir");

builder.Services.AddControllers();
builder.Services.AddEndpointsApiExplorer();
builder.Services.AddSwaggerGen();
builder.Services.AddSingleton<JetsonClientService>();
builder.Services.AddSingleton<SceneCatalogService>();
builder.Services.AddHttpClient<DarkVesselMatchService>((sp, client) =>
{
    var baseUrl = sp.GetRequiredService<IConfiguration>()["DarkVessel:BaseUrl"];
    if (!string.IsNullOrWhiteSpace(baseUrl))
        client.BaseAddress = new Uri(baseUrl);
});

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
