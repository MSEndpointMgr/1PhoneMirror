using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.ApplicationInsights;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Azure.Functions.Worker;
using Microsoft.Extensions.Logging;

namespace OnePhoneMirror.Telemetry;

/// <summary>
/// POST /ping — opt-in anonymous launch beacon from the 1PhoneMirror desktop app.
/// Body: { "install_id": "&lt;guid&gt;", "version": "0.3.8", "os_build": "26100.1234" }
/// No IP is stored. No hostname, no username, no MAC.
/// </summary>
public class PingFunction
{
    private static readonly Regex GuidRx = new(
        @"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$",
        RegexOptions.Compiled);
    private static readonly Regex VersionRx = new(@"^[0-9]{1,3}(\.[0-9]{1,3}){1,3}$", RegexOptions.Compiled);
    private static readonly Regex OsBuildRx = new(@"^[0-9]{1,6}(\.[0-9]{1,6})?$", RegexOptions.Compiled);

    private readonly TelemetryClient _telemetry;
    private readonly ILogger<PingFunction> _log;

    public PingFunction(TelemetryClient telemetry, ILogger<PingFunction> log)
    {
        _telemetry = telemetry;
        _log = log;
    }

    public record LaunchPing(string? install_id, string? version, string? os_build);

    [Function("Ping")]
    public async Task<IActionResult> Run(
        [HttpTrigger(AuthorizationLevel.Anonymous, "post", Route = "ping")] HttpRequest req)
    {
        LaunchPing? body;
        try
        {
            body = await JsonSerializer.DeserializeAsync<LaunchPing>(
                req.Body,
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        }
        catch (JsonException)
        {
            return new BadRequestObjectResult(new { error = "invalid_json" });
        }

        if (body is null
            || string.IsNullOrWhiteSpace(body.install_id) || !GuidRx.IsMatch(body.install_id)
            || string.IsNullOrWhiteSpace(body.version)    || !VersionRx.IsMatch(body.version)
            || string.IsNullOrWhiteSpace(body.os_build)   || !OsBuildRx.IsMatch(body.os_build))
        {
            return new BadRequestObjectResult(new { error = "invalid_payload" });
        }

        var country = req.Headers["CF-IPCountry"].FirstOrDefault()
                   ?? req.Headers["X-Azure-ClientIP-Country"].FirstOrDefault()
                   ?? "??";

        _telemetry.TrackEvent("Launch", new Dictionary<string, string>
        {
            ["install_id"] = body.install_id.ToLowerInvariant(),
            ["version"]    = body.version,
            ["os_build"]   = body.os_build,
            ["country"]    = country
        });

        _log.LogInformation("Launch ping v={Version} os={OsBuild}", body.version, body.os_build);
        return new OkObjectResult(new { ok = true });
    }
}
