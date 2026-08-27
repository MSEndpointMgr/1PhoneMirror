# 1PhoneMirror Telemetry

A minimal Azure backend that gives the project two things the public winget repo and GitHub Releases page cannot:

1. **`GET /dl/{version}/{file}`** — logs the request (version, client class, country, UA) then 302-redirects to the real GitHub Release asset. Used as the `InstallerUrl` in the winget manifest so that **every winget install and every direct browser download is counted**.
2. **`POST /ping`** — opt-in anonymous "the app launched" beacon from the desktop client. Body: `{ "install_id": "<guid>", "version": "0.4.x", "sessions_ios": 0, "minutes_ios": 0, "sessions_android": 0, "minutes_android": 0, "screenshots_ios": 0, "screenshots_android": 0, "annotations_ios": 0, "annotations_android": 0, "ocr_copies_ios": 0, "ocr_copies_android": 0, "recordings_ios": 0, "recordings_android": 0 }`. Lets us measure **daily active installs**, **version adoption**, and aggregate **iOS vs. Android feature usage** — not just downloads.

A third route, `GET /healthz`, is just an uptime probe.

## What gets stored

| Field | Source | Notes |
|---|---|---|
| `version`, `file` | `/dl` URL path | |
| `client` | derived from User-Agent | `winget` / `browser` / `curl` / `powershell` / `other` |
| `country` | `CF-IPCountry` / `X-Azure-ClientIP-Country` header | 2-letter code only |
| `install_id` | client-generated GUID, stored once in `%LOCALAPPDATA%\1PhoneMirror\install_id.txt` | not a user ID — uninstall + reinstall = new ID |
| `sessions_ios`, `minutes_ios`, `sessions_android`, `minutes_android` | client's local usage log (`opm::UsageLog`) | lifetime totals as of the ping; mirroring session counts/minutes, no device identifiers |
| `screenshots_*`, `annotations_*`, `ocr_copies_*`, `recordings_*` | client's local usage log | lifetime totals per feature, split iOS vs. Android |

**Not stored:** IP address, hostname, username, MAC, Windows build number, screen contents, anything from the mirrored phone.

App Insights retention is set to **730 days interactive + 4383 days archive (~12 years total)** — the platform maximum; Azure does not offer true unlimited retention. The workspace is capped at **1 GB/day** so a stuck client cannot generate a surprise bill.

## Architecture

```
winget client / browser
       │  GET https://stats.1phonemirror.app/dl/0.3.8/1PhoneMirror-0.3.8.msi
       ▼
Function App (Flex Consumption, .NET 8 isolated)
       │  TrackEvent("Download", {version, client, country, ua})
       ▼
Application Insights ── Log Analytics workspace (90d, 1 GB/day cap)
       ▲
       │  KQL
Workbook "1PhoneMirror — Usage"
```

## Deploy

Prerequisites: `azd`, .NET 8 SDK, an Azure subscription.

```pwsh
cd telemetry
azd auth login
azd up    # pick env name e.g. 'opm-tel-prod', region e.g. 'westeurope'
```

Outputs include `FUNCTION_APP_URL`. Smoke-test:

```pwsh
$base = (azd env get-values | Select-String FUNCTION_APP_URL).ToString().Split('=')[1].Trim('"')
Invoke-RestMethod "$base/healthz"
Invoke-WebRequest "$base/dl/0.3.8/1PhoneMirror-0.3.8.msi" -MaximumRedirection 0 -SkipHttpErrorCheck
Invoke-RestMethod -Method Post "$base/ping" -ContentType application/json `
    -Body '{"install_id":"11111111-1111-1111-1111-111111111111","version":"0.3.8","sessions_ios":1,"minutes_ios":5,"sessions_android":0,"minutes_android":0,"screenshots_ios":2,"screenshots_android":0,"annotations_ios":0,"annotations_android":0,"ocr_copies_ios":0,"ocr_copies_android":0,"recordings_ios":0,"recordings_android":0}'
```

Subsequent code-only deploys: `azd deploy`.

## Import the Workbook

```pwsh
# Azure Portal → Application Insights → Workbooks → New → "</>" Advanced Editor
# Paste contents of telemetry/workbook.json → Apply → Save.
```

## Cutting over winget + GitHub Releases

Once a stable custom domain (e.g. `dl.1phonemirror.app`) is in front of the Function App:

1. **winget manifest** — set `InstallerUrl` to `https://dl.1phonemirror.app/dl/0.3.8/1PhoneMirror-0.3.8.msi` instead of the raw GitHub URL.
2. **README install instructions** — keep both URLs documented so people can still go direct if they prefer.

Until the custom domain exists, use the raw `*.azurewebsites.net` URL.

## Cost shape

Expected at this project's scale (≈100–500 downloads/month, ≈50 daily-active installs):

| Resource | Tier | Est. monthly |
|---|---|---|
| Function App | Flex Consumption FC1, ~0 GB-s | $0 (free grant) |
| Storage | Standard LRS, <1 GB | <$0.05 |
| Log Analytics + App Insights | <100 MB ingest | $0 (5 GB/month free) |
| **Total** | | **~$0** |

The 1 GB/day cap on the workspace is the hard ceiling — even a flood of requests cannot run the bill above ~$2.30/day.

## Adding the launch ping to the desktop app

Not done yet — this branch only ships the backend. The client side will:

* generate a GUID on first launch into `%LOCALAPPDATA%\1PhoneMirror\install_id.txt`
* add a Settings toggle `Send anonymous launch ping` (default OFF for the first release; flip to ON-by-default once the feature has shipped quietly for a version)
* POST `/ping` once per launch, fire-and-forget, 2 s timeout, never blocks startup
