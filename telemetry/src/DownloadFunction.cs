using System.Net;
using Microsoft.ApplicationInsights;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Azure.Functions.Worker;
using Microsoft.Azure.Functions.Worker.Http;
using Microsoft.Extensions.Logging;

namespace OnePhoneMirror.Telemetry;

/// <summary>
/// /dl/{version}/{file} → log + 302 to GitHub release asset.
/// Lets us count downloads and split winget vs browser via User-Agent.
/// </summary>
public class DownloadFunction
{
    private readonly TelemetryClient _telemetry;
    private readonly ILogger<DownloadFunction> _log;
    private readonly string _releaseBaseUrl;

    public DownloadFunction(TelemetryClient telemetry, ILogger<DownloadFunction> log)
    {
        _telemetry = telemetry;
        _log = log;
        _releaseBaseUrl = Environment.GetEnvironmentVariable("RELEASE_BASE_URL")
            ?? "https://github.com/MSEndpointMgr/1PhoneMirror/releases/download";
    }

    [Function("Download")]
    public IActionResult Run(
        [HttpTrigger(AuthorizationLevel.Anonymous, "get", "head", Route = "dl/{version}/{file}")] HttpRequest req,
        string version,
        string file)
    {
        // Reject obvious path traversal / non-MSI / non-EXE garbage
        if (!IsSafeSegment(version) || !IsSafeSegment(file))
        {
            _log.LogWarning("Rejected download with unsafe segments: version={Version} file={File}", version, file);
            return new BadRequestResult();
        }

        var ua = req.Headers.UserAgent.ToString();
        var client = ClassifyClient(ua);
        // Cloudflare / Front Door supply country code header; fall back to "??"
        var country = req.Headers["CF-IPCountry"].FirstOrDefault()
                   ?? req.Headers["X-Azure-ClientIP-Country"].FirstOrDefault()
                   ?? "??";

        _telemetry.TrackEvent("Download", new Dictionary<string, string>
        {
            ["version"] = version,
            ["file"]    = file,
            ["client"]  = client,
            ["ua"]      = Truncate(ua, 256),
            ["country"] = country
        });

        var target = $"{_releaseBaseUrl}/v{version}/{file}";
        _log.LogInformation("Redirect {Client} -> {Target}", client, target);
        return new RedirectResult(target, permanent: false);
    }

    private static bool IsSafeSegment(string s) =>
        !string.IsNullOrWhiteSpace(s)
        && s.Length <= 100
        && !s.Contains("..")
        && !s.Contains('/')
        && !s.Contains('\\');

    private static string ClassifyClient(string ua)
    {
        if (string.IsNullOrEmpty(ua)) return "unknown";
        if (ua.StartsWith("winget-cli", StringComparison.OrdinalIgnoreCase)) return "winget";
        if (ua.Contains("Microsoft.DesktopAppInstaller", StringComparison.OrdinalIgnoreCase)) return "winget";
        if (ua.Contains("Mozilla", StringComparison.OrdinalIgnoreCase)) return "browser";
        if (ua.Contains("curl", StringComparison.OrdinalIgnoreCase)) return "curl";
        if (ua.Contains("PowerShell", StringComparison.OrdinalIgnoreCase)) return "powershell";
        return "other";
    }

    private static string Truncate(string s, int max) =>
        string.IsNullOrEmpty(s) ? "" : (s.Length <= max ? s : s[..max]);
}
