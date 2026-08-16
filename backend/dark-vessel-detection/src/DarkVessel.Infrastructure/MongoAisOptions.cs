namespace DarkVessel.Infrastructure;

public sealed class MongoAisOptions
{
    public const string SectionName = "Mongo";

    public string ConnectionString { get; set; } = "";

    public string Database { get; set; } = "ships";

    public string PositionsCollection { get; set; } = "ships_table";

    public string VesselsCollection { get; set; } = "vessels";

    public string CoverageCollection { get; set; } = "ais_coverage";

    public string TimeZone { get; set; } = "UTC";

    public int CandidateLimit { get; set; } = 5000;

    public bool EnsureIndexes { get; set; } = true;
}
