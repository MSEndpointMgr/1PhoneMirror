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

### 2. Create a `PUBLIC_RELEASE_TOKEN` (cross-repo publish)

A fine-grained PAT scoped to **only** `MSEndpointMgr/1PhoneMirror`:

- GitHub → your profile → **Settings** → **Developer settings** →
  **Personal access tokens** → **Fine-grained tokens** → **Generate new token**
- Resource owner: `MSEndpointMgr`
- Repository access: Only select repositories → `MSEndpointMgr/1PhoneMirror`
- Permissions: **Contents: Read and write**
- Expiration: 1 year (calendar reminder to rotate)

> If MSEndpointMgr requires PAT approval, an org admin must approve the token
> before it works. Falls back to a classic PAT with `repo` scope if needed.

Store it in the **private** source repo:

```powershell
gh secret set PUBLIC_RELEASE_TOKEN --repo SimonSkotheimsvik/1PhoneMirror
# paste the PAT when prompted
```

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

**Prerequisite:** the **public** release must already exist. Tag and push from
the private repo first to trigger `.github/workflows/release.yml`, which builds
the MSI and pushes it to `MSEndpointMgr/1PhoneMirror/releases/v0.2.0`:

```powershell
git tag v0.2.0
git push origin v0.2.0
# Wait for the "Build & Release MSI" workflow to finish (Actions tab).
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

> **Current status (2026-05):** `PUBLIC_RELEASE_TOKEN` has not been approved by
> the MSEndpointMgr org, so the automated release workflow cannot push to the
> public repo. **Until that PAT is approved, use the manual routine below.**
> The fully-automated routine is documented further down for the day approval lands.

### Step 1 — Bump the version

Edit [`CMakeLists.txt`](CMakeLists.txt):
```cmake
project(1PhoneMirror VERSION 0.2.5 LANGUAGES C CXX)
```

Update version strings in [`src/media/renderer.cpp`](src/media/renderer.cpp):
- Footer line 2 (`footer_line2_.push_back(seg(L" \u00B7 v0.2.5", ...`)
- Info panel header (`info_lines_.push_back(make_info(L"1PhoneMirror v0.2.5", ...`)
- Add a new entry at the top of the `version_lines_` block (date – version + one-liner)

### Step 2 — Build the MSI locally

```powershell
Stop-Process -Name 1PhoneMirror -Force -ErrorAction SilentlyContinue
.\package.ps1
# Produces dist\1PhoneMirror-0.2.5.msi
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

> The release CI workflow will trigger on the tag and currently fail at the
> cross-repo publish step (HTTP 403). Ignore that failure — the manual upload
> below replaces it.

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

## Fully-automated routine (when `PUBLIC_RELEASE_TOKEN` is approved)

Once an MSEndpointMgr org admin approves the fine-grained PAT (or you replace
it with a classic PAT that has org access), the workflow takes over from the
tag push:

1. `git tag v0.2.x && git push origin v0.2.x`
2. `.github/workflows/release.yml` runs on `windows-latest`, restores vcpkg
   cache, runs `package.ps1 -Version 0.2.x`, computes SHA256, and pushes the
   MSI to `MSEndpointMgr/1PhoneMirror/releases/v0.2.x` using
   `PUBLIC_RELEASE_TOKEN`.
3. `.github/workflows/winget.yml` fires `winget-releaser`, which downloads
   the public MSI, generates updated manifests, forks `microsoft/winget-pkgs`,
   and opens a PR — using `WINGET_TOKEN`.

Watch progress under the **Actions** tab. If the publish step fails again
with `HTTP 403: Resource not accessible by personal access token`, the PAT
is still not approved — fall back to the manual routine.

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
| Tag without `v` prefix (`0.2.1`) | Release workflow doesn't trigger | Tag is matched by `v*.*.*` |
| MSI filename mismatch | winget PR fails on download | `1PhoneMirror-<version>.msi` exactly |
| `WINGET_TOKEN` expired | "Submit to winget" fails 401 | Regenerate PAT, update secret |
| Changing `UpgradeCode` | Users get duplicate installs instead of upgrade | Never edit it — keep the GUID |
| Editing manifests in your fork while a PR is open | Conflicts | Let the workflow drive it; if needed, close the PR and rerun |
| First-time identifier never approved | Auto path can't run | See "One-time setup" — do `wingetcreate new` first |
| `PUBLIC_RELEASE_TOKEN` not approved by org | `release.yml` fails: `HTTP 403: Resource not accessible by personal access token` | Use the manual routine (Step 4 onwards) until org admin approves the PAT |

---

## File reference

- [`.github/workflows/release.yml`](.github/workflows/release.yml) — build + release on tag
- [`.github/workflows/winget.yml`](.github/workflows/winget.yml) — winget submit on release
- [`package.ps1`](package.ps1) — local MSI build (also called by CI)
- [`installer/1PhoneMirror.wxs`](installer/1PhoneMirror.wxs) — WiX 5 source
- [`CMakeLists.txt`](CMakeLists.txt) — single source of truth for version

---

## Optional improvements (later)

- **Code signing** — adds an Authenticode signing step in `release.yml` before SHA computation. Requires a code-signing cert (DigiCert / Sectigo / SignPath community).
- **`vcpkg.json` manifest mode** — pins exact dependency versions for reproducible CI builds and tighter cache keys.
- **Changelog file** — `CHANGELOG.md` consumed by `softprops/action-gh-release` for richer release notes.
- **Pre-release channel** — push tags `v0.2.1-beta1`; gate winget submission on a non-prerelease check.
