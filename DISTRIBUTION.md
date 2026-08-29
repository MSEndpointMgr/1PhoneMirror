# Distribution Routine — 1PhoneMirror

How to ship a new version. Source lives in the **private** repo
`SimonSkotheimsvik/1PhoneMirror`; the MSI is published to the **public** repo
`MSEndpointMgr/1PhoneMirror` (which is what winget points at).

> **Package identifier:** `MSEndpointMgr.1PhoneMirror`
> **Source repo (private):** `SimonSkotheimsvik/1PhoneMirror`
> **Release repo (public):** `MSEndpointMgr/1PhoneMirror`
> **MSI naming:** `1PhoneMirror-<version>.msi`
> **Installer URL:** `https://github.com/MSEndpointMgr/1PhoneMirror/releases/download/v<version>/1PhoneMirror-<version>.msi`

---

## One-time setup (do this once)

### 1. Create the public release repo

In the **MSEndpointMgr** GitHub org, create a new **public** repository named
`1PhoneMirror` with a GPL-3.0 license and a minimal user-facing README
(install via winget + a screenshot — no source code).

The public repo's `main` branch must exist before the first release (the
release workflow creates tags off `main`).

### 2. Public-repo publish access (manual `gh` login)

Releases are published by hand with `gh release create`, so you just need a
`gh` login on an account with **Write** access to `MSEndpointMgr/1PhoneMirror`
(`gh auth status` should show scope `repo`).

> The old `PUBLIC_RELEASE_TOKEN` repo secret (a fine-grained PAT for CI
> cross-repo publishing) is **no longer used** — CI does not publish releases.
> You can delete that secret if it still exists.

### 3. Create a `WINGET_TOKEN` for winget-releaser

A **classic** PAT on **your personal account** (winget-releaser forks
`microsoft/winget-pkgs` under your user):

- Scope: **`public_repo`** only
- Expiration: 1 year

```powershell
gh secret set WINGET_TOKEN --repo SimonSkotheimsvik/1PhoneMirror
```

### 4. Make the FIRST winget submission manually

The package identifier `MSEndpointMgr.1PhoneMirror` does not yet exist in the
catalog, so the automated update path can't run.

**Prerequisite:** the **public** release must already exist. Build and sign
the MSI locally, then publish it by hand (CI does NOT build or release):

```powershell
.\scripts\release.ps1 -Version 0.2.0   # builds + Azure Trusted Signs + hashes the exact MSI
gh release create v0.2.0 "dist\1PhoneMirror-0.2.0.msi" `
    --repo MSEndpointMgr/1PhoneMirror `
    --title "1PhoneMirror 0.2.0" `
    --notes "<release notes>"
# Confirm the asset exists at:
#   https://github.com/MSEndpointMgr/1PhoneMirror/releases/tag/v0.2.0
```

Once the public release is live:

```powershell
winget install Microsoft.WingetCreate

wingetcreate new https://github.com/MSEndpointMgr/1PhoneMirror/releases/download/v0.2.0/1PhoneMirror-0.2.0.msi
```

The wizard asks for:

| Field | Value |
|---|---|
| PackageIdentifier | `MSEndpointMgr.1PhoneMirror` |
| PackageVersion | `0.2.0` |
| Publisher | `Simon Skotheimsvik` |
| PackageName | `1PhoneMirror` |
| Moniker | `1phonemirror` |
| Author | `Simon Skotheimsvik` |
| License | `GPL-3.0` |
| ShortDescription | Wireless screen mirroring for iOS (AirPlay) and Android (scrcpy) on Windows. |
| Homepage | `https://msendpointmgr.com/1PhoneMirror` |
| Tags | `mirror`, `airplay`, `scrcpy`, `screen-mirroring`, `iOS`, `android` |
| InstallerType | `wix` |
| Scope | `machine` |

Validate, sandbox-test, then submit:

```powershell
winget validate .\manifests\m\MSEndpointMgr\1PhoneMirror\0.2.0\
winget install --manifest .\manifests\m\MSEndpointMgr\1PhoneMirror\0.2.0\

wingetcreate submit --token <PAT> .\manifests\m\MSEndpointMgr\1PhoneMirror\0.2.0\
```

Wait for the moderator at `microsoft/winget-pkgs` to merge the PR (usually a few hours to a day).

---

## Standard release routine (every new version)

> **Release model (2026-08):** CI does **not** build or release the MSI.
> The signed MSI produced locally by `scripts/release.ps1` (Azure Trusted
> Signing) is the single source of truth and is published to GitHub Releases
> by hand. This is the ONLY routine — there is no automated tag-triggered
> release. The manual steps below are the release process.

### Step 1 — Bump the version

Edit [`CMakeLists.txt`](CMakeLists.txt):
```cmake
project(1PhoneMirror VERSION 0.2.5 LANGUAGES C CXX)
```

Update version strings in [`src/media/renderer.cpp`](src/media/renderer.cpp):
- Footer line 2 (`footer_line2_.push_back(seg(L" \u00B7 v0.2.5", ...`)
- Info panel header (`info_lines_.push_back(make_info(L"1PhoneMirror v0.2.5", ...`)
- Add a new entry at the top of the `version_lines_` block (date – version + one-liner)

### Step 2 — Build and sign the MSI locally

```powershell
Stop-Process -Name 1PhoneMirror -Force -ErrorAction SilentlyContinue
.\scripts\release.ps1 -Version 0.2.5
# Builds Release, signs the EXE + all bundled DLLs + the MSI via Azure Trusted
# Signing, then writes dist\1PhoneMirror-0.2.5.msi plus a .sha256 and
# .release.json computed from the EXACT signed file. Use -SkipSign for an
# unsigned test build.
```

Smoke-test: install, launch, exercise core features, uninstall.

### Step 3 — Commit, tag, push

```powershell
git add CMakeLists.txt src/media/renderer.cpp
git commit -m "release: 0.2.5 - <short summary>"
git tag v0.2.5
git push origin main
git push origin v0.2.5
```

> Pushing the tag does **not** trigger any CI build or release. The signed
> MSI you built in Step 2 is published manually in Step 4 below.

### Step 4 — Publish the MSI to the public repo (manual)

Requires a `gh` login on an account with **Write** access to
`MSEndpointMgr/1PhoneMirror` (`gh auth status` should show scope `repo`).

```powershell
gh release create v0.2.5 "dist\1PhoneMirror-0.2.5.msi" `
    --repo MSEndpointMgr/1PhoneMirror `
    --title "1PhoneMirror 0.2.5" `
    --notes "<release notes>"
```

Verify the asset is reachable (winget will download from here):

```powershell
$url = "https://github.com/MSEndpointMgr/1PhoneMirror/releases/download/v0.2.5/1PhoneMirror-0.2.5.msi"
(Invoke-WebRequest -Uri $url -Method Head -MaximumRedirection 5).StatusCode  # expect 200
(Get-FileHash "dist\1PhoneMirror-0.2.5.msi" -Algorithm SHA256).Hash
```

### Step 5 — Submit the winget update

The package identifier `MSEndpointMgr.1PhoneMirror` is already in the
catalog (first submission was 0.2.1), so use `wingetcreate update`. If your
`gh` token has `repo` (or `public_repo`) scope, no `--token` argument is
needed — `wingetcreate` uses it automatically.

```powershell
wingetcreate update MSEndpointMgr.1PhoneMirror `
    --version 0.2.5 `
    --urls "https://github.com/MSEndpointMgr/1PhoneMirror/releases/download/v0.2.5/1PhoneMirror-0.2.5.msi" `
    --submit
```

Optional dry-run first (no PR opened, just generates and validates manifests
under `.\manifests\`):

```powershell
wingetcreate update MSEndpointMgr.1PhoneMirror `
    --version 0.2.5 `
    --urls "https://github.com/MSEndpointMgr/1PhoneMirror/releases/download/v0.2.5/1PhoneMirror-0.2.5.msi" `
    --out .\manifests\
winget validate .\manifests\manifests\m\MSEndpointMgr\1PhoneMirror\0.2.5\
```

### Step 6 — Watch the PR and wait for merge

```powershell
gh pr list --repo microsoft/winget-pkgs --search "MSEndpointMgr.1PhoneMirror 0.2.5" --state all
```

Microsoft's bot validates automatically; a moderator merges (usually within
hours for established packages). Monitor for failure comments (installer URL
unreachable, hash mismatch).

### Step 7 — Verify availability

Once the PR is merged, the manifest is picked up by the next index rebuild.
Typical propagation:

- **`winget` CLI** — usually 15–60 minutes after merge
- **winget.run / winstall.app** — a few hours
- **Microsoft Store surface** — up to 24–48 hours

```powershell
winget source update
winget search MSEndpointMgr.1PhoneMirror
winget show MSEndpointMgr.1PhoneMirror
```

---

## CI does not build or release (by design)

There is intentionally **no** automated tag-triggered release. The previous
"Build & Release MSI" workflow was removed because a CI rebuild produces a
differently-hashed, **unsigned** MSI that would clobber the signed GitHub
Release and break winget hash validation.

- [`.github/workflows/release.yml`](.github/workflows/release.yml) is now a
  **manual `workflow_dispatch` validation build only**. It compiles the app and
  uploads an *unsigned* artifact for debugging. It never creates or overwrites
  a GitHub Release.
- [`.github/workflows/winget.yml`](.github/workflows/winget.yml) is a **manual
  `workflow_dispatch`** that runs `wingetcreate` against an already-published
  signed release. It no longer chains off any CI build.

Releases are produced only by the local, signed routine above.

---

## Verification checklist before tagging

- [ ] `CMakeLists.txt` `project(... VERSION X.Y.Z)` matches the planned tag
- [ ] Version-history panel updated with a one-liner for the new version
- [ ] Footer + info-panel version strings match (in `src/media/renderer.cpp`)
- [ ] `installer/1PhoneMirror.wxs` `UpgradeCode` **unchanged** (always `6F4E6B5C-2E4A-4B1F-9D2E-7B5C8F3A1E10`)
- [ ] Local `.\package.ps1` succeeds
- [ ] Smoke-test the MSI: install on a clean VM, run, uninstall

---

## Things that will break the pipeline

| Mistake | Symptom | Fix |
|---|---|---|
| MSI filename mismatch | winget PR fails on download | `1PhoneMirror-<version>.msi` exactly |
| `WINGET_TOKEN` expired | "Submit to winget" fails 401 | Regenerate PAT, update secret |
| Changing `UpgradeCode` | Users get duplicate installs instead of upgrade | Never edit it — keep the GUID |
| Editing manifests in your fork while a PR is open | Conflicts | Let `wingetcreate` drive it; if needed, close the PR and rerun |
| First-time identifier never approved | Submission can't run | See "One-time setup" — do `wingetcreate new` first |
| Publishing a different MSI than the one you hashed | winget hash mismatch on install | Always publish the exact file `release.ps1` hashed; re-run `wingetcreate` if the asset changed |

---

## File reference

- [`.github/workflows/release.yml`](.github/workflows/release.yml) — manual validation build only (no release)
- [`.github/workflows/winget.yml`](.github/workflows/winget.yml) — manual winget submission against a published release
- [`scripts/release.ps1`](scripts/release.ps1) — local build + Azure Trusted Signing + exact-MSI hash (authoritative)
- [`scripts/trusted-signing.ps1`](scripts/trusted-signing.ps1) — Azure Trusted Signing helper (dlib download, signtool)
- [`package.ps1`](package.ps1) — local MSI build/stage/sign
- [`installer/1PhoneMirror.wxs`](installer/1PhoneMirror.wxs) — WiX 5 source
- [`CMakeLists.txt`](CMakeLists.txt) — single source of truth for version

---

## Optional improvements (later)

- **`vcpkg.json` manifest mode** — pins exact dependency versions for reproducible CI builds and tighter cache keys.
- **Changelog file** — `CHANGELOG.md` consumed by `softprops/action-gh-release` for richer release notes.
- **Pre-release channel** — push tags `v0.2.1-beta1`; gate winget submission on a non-prerelease check.
