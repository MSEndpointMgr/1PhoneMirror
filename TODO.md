# TODO — Feature backlog

Scribbled ideas, framed around the actual audience: Intune / endpoint
admins demoing enrollment, MAM, Conditional Access, Autopilot, and
writing KB articles. Nothing here is committed work — pull items out
and convert to GitHub Issues when they become real.

## 1. Step recorder for phones
Windows already has *Steps Recorder* for PCs. Nothing equivalent exists
for iOS / Android. While mirroring, detect stable frames (no pixel
delta for ~500 ms) → auto-capture + number them → on stop, export a
**single Markdown / DOCX / PDF** with screenshots + the OCR'd headline
of each screen as the step caption. Ships as
`My-Enrollment-Walkthrough.docx` ready to paste into a KB.

## 2. Real-time PII redaction (live AND in the encoded recording)
OCR is already wired up. Run it on every Nth frame, detect:
- UPNs / email addresses
- Tenant IDs (GUID pattern)
- Phone numbers, IMEI, serial numbers, MAC addresses
- Device names like `iPhone-of-Simon`
- Authenticator 6-digit codes

…and pixelate **in the live preview AND the encoded MP4** before it
hits disk. Toggle per pattern in Settings. Removes the re-record cycle
after a UPN slips through.

## 3. Tenant / brand neutraliser for demos
Detect Intune Company Portal / Microsoft sign-in / CA prompts via OCR
template matching. On a hotkey, overlay a synthetic tenant: replace
tenant logo with a placeholder, swap `contoso.onmicrosoft.com`
everywhere it appears, replace the user's display name with
`Alex Demo`. The phone still does the real flow; viewers see a
neutered version. Record once, reuse across customers.

## 4. Side-by-side compare (iOS vs Android, or v1 vs v2 policy)
Two AirPlay devices, or one AirPlay + one Android, rendered as a
**synchronized split-screen with a shared timeline**. Press record →
both streams go into one MP4 stacked horizontally. Use case: "Here's
the same Conditional Access policy on iOS and Android — note the prompt
difference at 0:42." Today people record twice and edit in Premiere.

## 5. Audit-grade session log
Every connect / disconnect, every screen transition (frame-delta +
OCR title), every screenshot / recording — written to a sidecar
`session-<ts>.jsonl` next to the recording. For consultants billing
hourly or compliance teams needing evidence that a config change was
demonstrated, this is the proof artifact. Optional: HMAC-sign each
line with a per-session key so the log is tamper-evident.

## 6. Replay buffer ("show me the last 30 seconds")
Always-on rolling 60-second RGBA ring in memory (cheap at 360p).
Hotkey: "save the last X seconds as MP4." Solves the universal demo
problem: the interesting thing happened *before* you hit record.
Twitch / Shadowplay have had this for a decade; no mirroring app does.

## 7. Built-in MDM scenario library
Curate a YAML of well-known enrollment flows (Apple ADE, Android
Enterprise zero-touch, Samsung Knox, Autopilot for HoloLens, …) — each
entry has expected OCR strings per step. While recording, app shows a
**live checklist** in the corner: "Step 3/8: Company Portal install —
detected ✓". Trainees know they're on track; recordings get chapter
markers for free. The YAML is community-editable in this repo —
MSEndpointMgr can crowdsource it.

## 8. Demo-presenter chroma-key (virtual webcam)
Composite the phone bezel onto a transparent background, broadcast it
as a **virtual webcam** (DirectShow filter or use OBS Virtual Camera
SDK). User in Teams / Zoom puts the phone frame on top of their own
face cam in PiP. No OBS, no scene config, no second monitor needed.
The presenter just picks "1PhoneMirror Camera" in Teams.

## 9. Webcam drawer — capture the hands & hardware
Screen mirroring fundamentally can't show what the user's hands and
the device hardware are doing. A webcam panel solves that, and it's
exactly the missing piece for the MDM scenarios where admins struggle
to document today:

- **Biometric enrollment** — Face ID / fingerprint setup looks like a
  blank screen on the mirror; the webcam shows the actual head-turn
  / finger-rotate motion.
- **Hardware-button flows** — Apple Configurator trust prompts, DFU /
  recovery (vol-up + vol-down + side), force-restart, physical wipe.
- **eSIM / NFC provisioning** — scanning a QR sheet, NFC tap for
  Android zero-touch.
- **SIM swap, USB-C pairing, Apple Watch pairing dance.**
- **Gestures** — three-finger swipe, AssistiveTouch, Samsung Knox
  guard gesture — flat taps don't communicate them.

### Layout: bottom-fold drawer
The webcam is a **landscape panel that folds out from the bottom of
the phone bezel**, because phones mirror in portrait and most webcams
are 16:9 landscape — the shapes complement each other and the overall
window stays compact.

- Drawer collapsed by default (hotkey `W` or a small handle on the
  bottom bezel toggles it).
- When open, the panel sits flush under the phone, full bezel width,
  webcam-native aspect (typically 16:9).
- Drawer state, source GUID, mirror-flip, and visibility persisted
  per-source in `settings.ini` (e.g. iPhone defaults to drawer-open,
  Android to closed).

### Recording / composition
- When MP4/GIF recording is active and the drawer is open, the encoded
  output contains the composited (phone + webcam) frame as a single
  video — no post-production needed.
- Independent toggles: webcam can run without recording (live demo)
  and recording can run without webcam (current behaviour).
- Mic capture as part of the same drawer — feeds narrated walkthroughs
  and ties into the Step Recorder (#1) for spoken step captions.

### Plumbing that already helps
- `media::Renderer` already composites multiple SDL textures (phone
  frame + content) — webcam quad is a small extension, not a redesign.
- `media::Recorder` is format-agnostic on input — feed it a composite
  RGBA buffer and you're done.
- Bezel hover/tooltip + drawer animation patterns are established
  (log drawer, version panel).
- Settings persistence is established
  ([include/opm/settings.h](include/opm/settings.h) +
  [src/settings.cpp](src/settings.cpp)).

### Gotchas to design around
- **Latency mismatch.** Webcam ~50–100 ms, AirPlay ~80–150 ms — if
  they drift the recording shows the physical button press *after* the
  on-screen response. Small jitter buffer + timestamp matching, or
  accept it for v1.
- **Aspect handling.** 16:9 webcam vs 9:19 phone — easiest fix is
  fixed aspect "card" matching webcam native, scaled to drawer width.
- **Privacy / red light.** Windows shows the Camera indicator and
  writes to the Camera privacy log; note in
  [PRIVACY.md](PRIVACY.md) that capture is local-only.
- **Permission UX.** Win11 prompts for Camera access on first use.
  Handle denial: toast "Camera blocked — enable in Windows Privacy
  Settings → Camera".
- **CPU.** Two video pipelines + encoder pushes laptop fans. Use
  Media Foundation directly (not OpenCV/FFmpeg dshow); drop webcam
  frames if the encoder backpressures.
- **Strictly optional.** Many users record sensitive flows where
  adding their face is the *last* thing they want — opt-in always.

### Synergy with other items
- #1 Step Recorder → physical-action panel per step ("place finger on
  Touch ID").
- #5 Audit log → `webcam_active=true` flag per session.
- #7 Scenario library → step entries flag "show camera" for biometric
  / hardware steps; drawer auto-opens at those steps.

---

## Explicitly NOT planning (already evaluated, low value here)

### Code signing — Azure Trusted Signing (attempted May 2026, abandoned)
Spent a full sprint trying to set up Azure Trusted Signing (now "Artifact
Signing") for MSI code-signing. Challenges that made it impossible **from
Norway** at this time:

1. **Sponsored Azure subscriptions are blocked.** The MVP Subscription
   (`Sponsored_2016-01-01` quota) failed the billing pre-check with
   "Unable to connect to the billing service." FAQ explicitly excludes
   free, trial, and sponsored offers.
2. **VSE subscription works** (`MSDN_2014-09-01`) — Phase 1 infra
   (account, UAMI, OIDC federated creds, RBAC) deployed successfully.
3. **Norway is not supported for Public Trust identity validation.**
   The country dropdown only lists USA, Canada, EU member states, and UK.
   Norway is EEA (not EU) and does not appear. Private Trust validation
   completed fine but can't be used for a `PublicTrust` certificate
   profile — deployment fails with `BadResourceOperation:
   PublicTrust certificate requires Public identity validation`.
4. **No programmatic workaround exists.** Spending-limit removal can
   only be done in the portal; identity validation is a manual review.
5. **Support ticket filed** requesting Norway/EEA support — no
   resolution as of June 2026. Revisit if Microsoft expands the
   supported country list.

**All Azure resources have been deleted.** The `signing/` folder and
branch have been removed from the repo. The release workflow no longer
references Trusted Signing. SignPath references were also removed
earlier (their free OSS tier rejected the project).

**Alternative paths if code signing is needed later:**
- Wait for Norway/EEA to be added to Artifact Signing Public Trust.
- Register a branch office in an EU country (e.g. Sweden) with its
  own DUNS — use that entity for validation.
- Buy a traditional OV code-signing certificate from a CA (DigiCert,
  Sectigo, GlobalSign) — more expensive, manual renewal, but no
  country restriction.

---

## Explicitly NOT planning (already evaluated, low value here)
- **Touch passthrough on Android** — scrcpy already does it natively;
  users who need it can run scrcpy directly.
- **More bezel themes / skins** — pure cosmetics, no business value.
- **WebRTC restream to browsers** — niche; the people who need it
  already have OBS + NDI.
- **Cloud sync of recordings** — privacy nightmare for an MDM-adjacent
  tool; conflicts with the posture in [PRIVACY.md](PRIVACY.md).
