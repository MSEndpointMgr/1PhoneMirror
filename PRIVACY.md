# Privacy Policy

*Last updated: 19 May 2026*

This document describes exactly what data 1PhoneMirror collects from
the user's PC, where it is sent, and how to opt out.

## What 1PhoneMirror sends over the network

1PhoneMirror is a screen-mirroring receiver. Most of its network
activity is **local** — between your PC and your phone over your own
Wi-Fi (AirPlay RTSP, scrcpy, mDNS, Miracast). None of that local
mirror traffic ever leaves your network.

In addition to local mirroring, 1PhoneMirror makes two outbound
internet requests against systems operated by the maintainer:

### 1. Launch ping (anonymous usage telemetry)

| Field | Value |
|---|---|
| **Endpoint** | `https://func-3qykfcznbey62.azurewebsites.net/ping` (Azure Functions, operated by the maintainer) |
| **Method** | HTTP `POST` |
| **Sent** | Once per app launch, fire-and-forget (failure does not affect the app) |
| **Default** | **Enabled** |
| **Opt out** | Open the app, press `S` (Settings), toggle off **Telemetry**. The setting is saved to `%APPDATA%\1PhoneMirror\settings.ini` (`telemetry_enabled=0`) and respected on the next launch. |
| **Payload** | JSON: `{"install_id": "<random GUID>", "version": "0.4.x", "os_build": "<Windows build number>"}` |
| **`install_id`** | Generated locally on first launch, stored at `%LOCALAPPDATA%\1PhoneMirror\install_id.txt`. Deliberately placed in **LocalAppData** so it does **not** roam to other PCs. Delete the file to reset it. |
| **Purpose** | Counting active installs and crash-free launches per version so the maintainer can prioritise fixes. |
| **What is *not* sent** | No username, no machine name, no email, no IP address logged at the application layer (the server only sees what every HTTP request exposes — IP visible in transit, not stored), no phone information, no mirrored content, no screenshots, no recordings. |
| **Retention** | Pings are stored as Azure Application Insights events under the maintainer's subscription and retained per the [Application Insights default retention](https://learn.microsoft.com/azure/azure-monitor/app/data-retention-privacy) (90 days unless changed). |
| **Third parties** | The endpoint is hosted on Microsoft Azure (data centre region as configured). Microsoft Azure's privacy policy applies in transit and at rest. |

### 2. Update check

| Field | Value |
|---|---|
| **Endpoint** | `https://api.github.com/repos/MSEndpointMgr/1PhoneMirror/releases/latest` |
| **Method** | HTTP `GET` |
| **Sent** | On startup and when you open the Info panel |
| **Default** | **Enabled** |
| **Opt out** | Block `api.github.com` in your firewall, or run 1PhoneMirror in an offline environment. There is no in-app toggle for this check. |
| **Payload** | None beyond the standard HTTP request. GitHub sees your IP per their [privacy policy](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement). |
| **Purpose** | Tell the user when a newer version is available. |

## What 1PhoneMirror does *not* do

- It does not transmit screenshots, recordings, mirrored frames, or
  anything captured from your phone.
- It does not transmit phone identifiers (serial numbers, IMEI, MAC
  addresses, device names) anywhere off your local network.
- It does not bundle third-party tracking, advertising, or analytics
  SDKs.
- It does not modify, scan, or collect data from files on your PC
  outside its own settings and screenshot output folders.

## Files written on your PC

| Location | Contents |
|---|---|
| `%APPDATA%\1PhoneMirror\settings.ini` | User preferences (bezel colour, recording format, **telemetry on/off**, etc.) |
| `%LOCALAPPDATA%\1PhoneMirror\install_id.txt` | The random GUID described above |
| `%LOCALAPPDATA%\1PhoneMirror\Crashes\` | Optional crash dumps if the app crashes (not transmitted; investigate or delete locally) |
| `Pictures\1PhoneMirror\` | Screenshots and recordings you create — not transmitted |

Uninstalling the MSI removes the installed program but does **not**
remove these per-user data folders. To wipe them, delete the
folders above after uninstall.

## Contact

Privacy questions or data-deletion requests can be raised through
the channels listed in [SECURITY.md](SECURITY.md), or by email to the
maintainer (see <https://linktr.ee/simonskotheimsvik>).

If you would like your `install_id` removed from the telemetry data
set, send the value from `%LOCALAPPDATA%\1PhoneMirror\install_id.txt`
and the maintainer will purge the matching events.
