namespace BaklavaBackend.Common;

public static class ConfigExtensions
{
    public static string Require(this IConfiguration config, string key)
    {
        var value = config[key];
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException(MissingMessage(key));
        return value;
    }

    public static void RequireAll(this IConfiguration config, params string[] keys)
    {
        var missing = keys.Where(k => string.IsNullOrWhiteSpace(config[k])).ToArray();
        if (missing.Length > 0)
            throw new InvalidOperationException(MissingMessage(string.Join(", ", missing)));
    }

    private static string MissingMessage(string keys) =>
        $"Missing configuration: {keys}. appsettings.json ships these blank on purpose; "
        + "put the real values in backend/appsettings.Development.json (git-ignored), "
        + "an environment variable (JetsonClient__ScriptPath), or user-secrets.";
}
