# tools/

Vendored runtime helpers required by 1PhoneMirror's Android (scrcpy)
receiver. Everything in this directory is copied next to
`1PhoneMirror.exe` on every build by a `POST_BUILD` step in
[../CMakeLists.txt](../CMakeLists.txt) and shipped inside the MSI by
[../package.ps1](../package.ps1).

At runtime, `src/app.cpp` looks for these paths relative to the EXE:

- `tools\adb\adb.exe`            - Android Debug Bridge client
- `tools\scrcpy-server.jar`      - scrcpy server pushed to the phone

If either is missing, Android mirroring is disabled but AirPlay /
Miracast / Cast still work.

## Contents

| Path | Source | License |
|---|---|---|
| `adb\adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll`, `fastboot.exe`, `etc1tool.exe`, `hprof-conv.exe`, `make_f2fs*.exe`, `mke2fs.exe`, `mke2fs.conf`, `libwinpthread-1.dll`, `sqlite3.exe`, `NOTICE.txt`, `source.properties` | Android SDK Platform Tools (Google) | Apache-2.0 + AOSP (see `adb\NOTICE.txt`) |
| `scrcpy-server.jar` | scrcpy (Genymobile) | Apache-2.0 |

Only `adb.exe` + its two `AdbWin*.dll`s and `scrcpy-server.jar` are
used by 1PhoneMirror; the rest are bundled simply because they're part
of the upstream platform-tools archive.

## Updating

1. Download the latest `platform-tools-latest-windows.zip` from
   https://developer.android.com/tools/releases/platform-tools and
   replace the contents of `adb\`.
2. Download the matching `scrcpy-server-vX.Y.jar` from
   https://github.com/Genymobile/scrcpy/releases and replace
   `scrcpy-server.jar` (rename it — the runtime expects this exact name).
3. Re-run `.\scripts\build.ps1` and smoke-test Android mirroring.
4. Commit the changes; the `.gitignore` is configured to allow `.exe`
   and `.dll` files under `tools/` past the global ignores.
