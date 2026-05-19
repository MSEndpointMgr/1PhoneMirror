# Security Policy

## Supported versions

Only the latest released version of 1PhoneMirror receives security fixes.
Older releases are archived as-is on the [Releases page](https://github.com/MSEndpointMgr/1PhoneMirror/releases)
for historical reference but will not receive backported patches.

| Version          | Supported          |
|------------------|--------------------|
| Latest release   | :white_check_mark: |
| Older releases   | :x:                |

## Reporting a vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Instead, report privately through one of:

1. **GitHub Security Advisories** (preferred) — open a draft advisory at
   <https://github.com/MSEndpointMgr/1PhoneMirror/security/advisories/new>.
2. **Email** — `security@msendpointmgr.com` (or, if unavailable,
   the maintainer directly at the address listed on
   <https://linktr.ee/simonskotheimsvik>).

When reporting, please include:

- A clear description of the issue and its impact.
- Steps to reproduce, ideally with a minimal proof-of-concept.
- The affected version (`1PhoneMirror.exe -V` or the Info panel in-app).
- Any relevant log output from the in-app log viewer (press `L`).

You can expect:

- **Acknowledgement** within 5 business days.
- A **triage decision** (accept / decline / need more info) within 14 days.
- For accepted issues, **coordinated disclosure**: a patched release is
  prepared and published, followed by public disclosure of the advisory
  once users have had a reasonable window to update.

## Signed releases

Starting with the first release published after onboarding to
[SignPath.io](https://signpath.io)'s open-source code signing program,
every official `1PhoneMirror-*.msi` published under
`MSEndpointMgr/1PhoneMirror` is digitally signed by the SignPath
Foundation certificate.

Verify a downloaded MSI with PowerShell:

```powershell
Get-AuthenticodeSignature .\1PhoneMirror-<version>.msi
```

The `SignerCertificate.Subject` should reference *SignPath Foundation*
and the status should be `Valid`. If the status is anything else
(`HashMismatch`, `NotSigned`, …), do not run the installer and please
report it via the channels above.

## Scope

In scope:

- The 1PhoneMirror MSI installer and the `1PhoneMirror.exe` it installs.
- The Windows Firewall rules and registry keys the installer creates.
- The network protocol implementations: AirPlay 2 (RTSP, FairPlay),
  scrcpy receiver, mDNS discovery, telemetry HTTPS ping.

Out of scope (but still appreciated as bug reports, not security reports):

- Upstream vulnerabilities in bundled third-party components (FFmpeg,
  SDL2, OpenSSL, scrcpy-server, adb) — please report those upstream
  first; we will rebuild against fixed versions.
- Issues that require an already-compromised local Windows account.

## Privacy

For details on what data the application collects (an opt-out launch
ping plus an update check) and how to disable it, see
[PRIVACY.md](PRIVACY.md).
