using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Azure.Functions.Worker;

namespace OnePhoneMirror.Telemetry;

/// <summary>GET /healthz — uptime probe.</summary>
public class HealthFunction
{
    [Function("Health")]
    public IActionResult Run(
        [HttpTrigger(AuthorizationLevel.Anonymous, "get", Route = "healthz")] HttpRequest req)
        => new OkObjectResult(new { ok = true, ts = DateTime.UtcNow });
}
