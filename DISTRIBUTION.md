# Distribution Routine — 1PhoneMirror

How to ship a new version to GitHub Releases and the public winget catalog.

> **Package identifier:** `MSEndpointMgr.1PhoneMirror`
> **Repo:** `SimonSkotheimsvik/1PhoneMirror`
> **MSI naming:** `1PhoneMirror-<version>.msi`

---

## One-time setup (do this once)

### 1. Create a GitHub Personal Access Token for winget

GitHub → **Settings** → **Developer settings** → **Personal access tokens** → **Tokens (classic)** → **Generate new token (classic)**:

- Scope: **`public_repo`** only
- Expiration: 1 year (calendar reminder to rotate)
- Copy the token

Add it to the repo:
- Repo → **Settings** → **Secrets and variables** → **Actions** → **New repository secret**
- Name: `WINGET_TOKEN`
- Value: the token

### 2. Make the FIRST winget submission manually

The package identifier `MSEndpointMgr.1PhoneMirror` does not exist in the catalog yet, so the automated update path can't run. After the first GitHub Release exists with the MSI attached:

```powershell
winget install Microsoft.WingetCreate

wingetcreate new https://github.com/SimonSkotheimsvik/1PhoneMirror/releases/download/v0.2.0/1PhoneMirror-0.2.0.msi
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
| Homepage | `https://github.com/SimonSkotheimsvik/1PhoneMirror` |
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

### Step 1 — Bump the version

Edit [`CMakeLists.txt`](CMakeLists.txt):
```cmake
project(1PhoneMirror VERSION 0.2.1 LANGUAGES C CXX)
```

Add a one-liner to the version-history panel in [`src/media/renderer.cpp`](src/media/renderer.cpp) (find the `version_lines_` block and add a new entry at the top).

Commit:
```powershell
git add CMakeLists.txt src/media/renderer.cpp
git commit -m "release: 0.2.1"
git push
```

### Step 2 — Tag and push

```powershell
git tag v0.2.1
git push origin v0.2.1
```

This triggers `.github/workflows/release.yml`:

1. Spins up `windows-latest`
2. Restores vcpkg cache (or builds FFmpeg/SDL2/OpenSSL on cache miss — slow first time)
3. Runs `package.ps1 -Version 0.2.1`
4. Computes SHA256
5. Creates GitHub Release `v0.2.1` and attaches `1PhoneMirror-0.2.1.msi`

Watch progress: **Actions** tab → "Build & Release MSI".

### Step 3 — winget PR opens automatically

When the Release is published, `.github/workflows/winget.yml` fires `winget-releaser`, which:

1. Downloads the new MSI from the release asset
2. Computes its hash
3. Forks `microsoft/winget-pkgs` (or reuses the existing fork)
4. Generates updated manifests under `manifests/m/MSEndpointMgr/1PhoneMirror/0.2.1/`
5. Opens a PR in `microsoft/winget-pkgs`

Check **Actions** → "Submit to winget" for the PR URL.

### Step 4 — Wait for merge

Microsoft's bot validates and a moderator reviews. Usually merged within hours for established packages. Monitor the PR for failure comments (e.g., installer URL unreachable, hash mismatch).

Once merged:
```powershell
winget search MSEndpointMgr.1PhoneMirror
winget install MSEndpointMgr.1PhoneMirror
```

---

## Manual fallback

If the automated workflow fails, rebuild and submit locally:

```powershell
# Build & package
.\package.ps1                    # produces dist\1PhoneMirror-X.Y.Z.msi

# Upload to a manually created GitHub Release matching the tag
# (gh CLI or web UI)

gh release create v0.2.1 dist\1PhoneMirror-0.2.1.msi `
    --title "1PhoneMirror 0.2.1" `
    --generate-notes

# Submit winget update
wingetcreate update MSEndpointMgr.1PhoneMirror `
    --version 0.2.1 `
    --urls https://github.com/SimonSkotheimsvik/1PhoneMirror/releases/download/v0.2.1/1PhoneMirror-0.2.1.msi `
    --submit `
    --token <PAT>
```

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
