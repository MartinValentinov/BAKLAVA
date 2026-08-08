using System.Text.RegularExpressions;

namespace BaklavaBackend.Common;

public static partial class Validation
{
    [GeneratedRegex(@"^[A-Za-z0-9_.\-]+$")]
    private static partial Regex SafeNameRegex();

    public static bool IsSafeName(string? name) =>
        !string.IsNullOrWhiteSpace(name) && SafeNameRegex().IsMatch(name);
}
