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
/// Body: { "install_id": "&lt;guid&gt;", "version": "0.4.x",
///          "sessions_ios": 0, "minutes_ios": 0, "sessions_android": 0, "minutes_android": 0,
///          "screenshots_ios": 0, "screenshots_android": 0,
///          "annotations_ios": 0, "annotations_android": 0,
///          "ocr_copies_ios": 0, "ocr_copies_android": 0,
///          "recordings_ios": 0, "recordings_android": 0 }
/// All numeric fields are lifetime totals from the client's local usage log
/// (see opm::UsageLog) — aggregate counts only, no per-device identifiers,
/// no timestamps, no content. No IP is stored. No hostname, no username, no MAC.
/// </summary>
public class PingFunction
{
    private static readonly Regex GuidRx = new(
        @"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$",
        RegexOptions.Compiled);
    private static readonly Regex VersionRx = new(@"^[0-9]{1,3}(\.[0-9]{1,3}){1,3}$", RegexOptions.Compiled);

    private readonly TelemetryClient _telemetry;
    private readonly ILogger<PingFunction> _log;

    public PingFunction(TelemetryClient telemetry, ILogger<PingFunction> log)
    {
        _telemetry = telemetry;
        _log = log;
    }

    public record LaunchPing(
        string? install_id, string? version,
        long sessions_ios = 0, long minutes_ios = 0,
        long sessions_android = 0, long minutes_android = 0,
        long screenshots_ios = 0, long screenshots_android = 0,
        long annotations_ios = 0, long annotations_android = 0,
        long ocr_copies_ios = 0, long ocr_copies_android = 0,
        long recordings_ios = 0, long recordings_android = 0);

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
            || string.IsNullOrWhiteSpace(body.version)    || !VersionRx.IsMatch(body.version))
        {
            return new BadRequestObjectResult(new { error = "invalid_payload" });
        }

        var country = req.Headers["CF-IPCountry"].FirstOrDefault()
                   ?? req.Headers["X-Azure-ClientIP-Country"].FirstOrDefault()
                   ?? "??";

        _telemetry.TrackEvent("Launch",
            properties: new Dictionary<string, string>
            {
                ["install_id"] = body.install_id.ToLowerInvariant(),
                ["version"]    = body.version,
                ["country"]    = country
            },
            metrics: new Dictionary<string, double>
            {
                ["sessions_ios"]         = body.sessions_ios,
                ["minutes_ios"]          = body.minutes_ios,
                ["sessions_android"]     = body.sessions_android,
                ["minutes_android"]      = body.minutes_android,
                ["screenshots_ios"]      = body.screenshots_ios,
                ["screenshots_android"]  = body.screenshots_android,
                ["annotations_ios"]      = body.annotations_ios,
                ["annotations_android"]  = body.annotations_android,
                ["ocr_copies_ios"]       = body.ocr_copies_ios,
                ["ocr_copies_android"]   = body.ocr_copies_android,
                ["recordings_ios"]       = body.recordings_ios,
                ["recordings_android"]   = body.recordings_android
            });

        _log.LogInformation("Launch ping v={Version}", body.version);
        return new OkObjectResult(new { ok = true });
    }
}

