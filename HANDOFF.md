# xlrbridge — Handoff & Implementation Spec

> **Status:** design complete, not yet implemented.
> **Audience:** an engineer (human or AI agent) picking this up to build the CLI from scratch.
> **Goal of this document:** be fully self-contained. You should be able to implement `xlrbridge` from this file alone, without the originating conversation.
> **Repository name:** `xlrbridge`
> **Official description / subtitle:** *fixing Discord's problem with handling xlr mics with fancy interfaces*
> **Language note:** written in English for a public OSS audience; the originating user speaks pt-BR.
> **Date:** 2026-06-30.

---

## 0. TL;DR

On macOS, apps like Discord that open a **USB audio interface directly** apply system voice-processing (AGC + echo/noise) and choke on **multichannel** interfaces (pro/XLR interfaces that expose many channels + loopback). Symptoms: the mic **chops** (periodic dropouts) and the **input volume is auto-limited** when you speak.

The fix is to never let the app open the interface directly. Instead, route **one chosen input channel** of the interface into a **clean 2-channel virtual device** (BlackHole) that the app uses as its mic. Routing must happen inside a **single CoreAudio clock domain** (an Aggregate Device with drift correction), or the audio drips/chops from clock drift.

`xlrbridge` is a small **native macOS CLI (C + CoreAudio/AudioToolbox)** that automates this: it creates a private Aggregate Device from `[interface + BlackHole]`, runs a routing IOProc (with digital gain), and installs a `launchd` agent so it survives login/reboot. It matches devices by **stable UID** (not index), which eliminates the fragility of the prototype.

A **working prototype** (Pure Data + manual Aggregate + launchd) exists and is documented in full in the Appendix. `xlrbridge` is the productized, dependency-light, robust version of that prototype.

---

## 1. The problem (motivation)

### 1.1 Symptoms
- Voice **chopping/stuttering** in Discord roughly every fraction of a second.
- Input level **auto-limited**: when the user starts talking, the captured level is pulled down, released, then pulled down again (classic AGC pumping).

### 1.2 What it is NOT
- **Not the specific interface.** Reproduced on a Topping E2x2 (2 analog in, exposes 6 channels incl. 4 loopback) AND on a Focusrite Scarlett 2i2 2nd gen (plain 2-in, no loopback). So it is not about channel count and not a Topping defect.
- **Not the network.** Google Meet works flawlessly on the same machine/network. Discord chops even in its **in-app mic test** (which is local, no network) and in the **browser** (WebRTC). QuickTime records the interface perfectly clean.
- **Not the user's audio hardware.** QuickTime capture of the interface is clean; the built-in mic works fine in Discord.

### 1.3 Root cause (diagnosis)
The failing common factor is **a USB audio interface opened directly by the app** (Discord, app or web). The working cases are **non-interface inputs** (built-in mic) or **a virtual device** (BlackHole/Loopback).

macOS applies a voice-processing path (the `VoiceProcessingIO` audio unit / system mic modes) to interfaces opened directly, which (a) drives AGC that limits the input level (often via the device's USB-exposed input volume control), and (b) glitches with the interface's stream, especially multichannel. Meet survives because WebRTC adapts and can fall back to TCP; Discord's path does not.

**The fix:** insert a **virtual audio device** between the interface and the app. The app opens the virtual device (a clean 2ch device); the OS never applies the interface voice-processing because the app isn't opening the interface. Something must copy the mic audio from the interface into the virtual device — that "something" is the core of this tool.

### 1.4 Why it must be one clock domain
The interface runs on its own hardware clock; the virtual device runs on the system clock. Copying samples between two free-running clocks accumulates drift; a naive realtime copy periodically under/overflows and **drops audio in chunks**. This was proven exhaustively with `ffmpeg` (see §1.5). The correct solution is to put the interface and the virtual device into one **Aggregate Device** with **drift correction**, so CoreAudio resamples the non-master sub-device to a single clock. Then a trivial sample copy inside that aggregate is glitch-free.

### 1.5 What was tried and rejected
- **ffmpeg** (`avfoundation` capture → `audiotoolbox` output to BlackHole): **rejected.** Its async resampler drops audio. Measured on 8 s of continuous tone: `aresample=async=1` → 12 short drops; `async=1000` → multi-second gaps; no async → total silence (timestamp issue); even routing through the aggregate → 9 regular ~0.5 s drops. ffmpeg is a file/stream transcoder, not a realtime device router; its realtime sink underruns. **Do not use ffmpeg for the routing.**
- **macOS Aggregate Device alone**: cannot reduce/route channels; it only combines devices. Still needs an engine to copy input→virtual-output. (But it IS the clock-sync mechanism — keep it.)
- **BlackHole alone**: it is a pure passthrough; it cannot capture a hardware mic by itself. It needs a router to feed it.
- **Pure Data**: **accepted for the prototype.** A real CoreAudio realtime engine; routing `adc~ → dac~` through the aggregate produced **0 drops**. `xlrbridge` replaces Pd with a native IOProc that does the same job without the Pd dependency.

---

## 2. The working prototype (current state — reference implementation)

This is what currently runs on the originating machine. `xlrbridge` should reproduce its behavior natively. Exact files are in the Appendix.

**Chain:** Topping E2x2 (input 1 = the mic, "Analogue 1") → Aggregate Device `[E2x2 + BlackHole 2ch]` (48 kHz, clock master = E2x2, drift correction ON for BlackHole) → Pure Data headless IOProc copies aggregate input ch0 → aggregate output ch6,7 (= BlackHole's outputs, which loop to BlackHole's inputs) with a +2 dB digital gain → Discord input = **BlackHole 2ch**.

**Aggregate channel map (8 in / 8 out):**
- Inputs: ch0 = Analogue 1 (mic), ch1 = Analogue 2, ch2–5 = E2x2 loopback, **ch6,7 = BlackHole inputs**.
- Outputs: ch0–5 = E2x2 playbacks, **ch6,7 = BlackHole outputs**.
- Routing: read input ch0 → write output ch6 and ch7. BlackHole loops output→input, so the app reading "BlackHole 2ch" gets the mic in stereo (dual-mono).

**Pure Data patch** (`~/.mic-bridge.pd`): `adc~ 1` → `*~ <gain>` → `dac~ 7 8` (Pd channels are 1-indexed; 7,8 = the BlackHole outputs in the aggregate). Auto-starts DSP via `loadbang → ; pd dsp 1`.

**Process management:** a `launchd` LaunchAgent `com.scoobert.micbridge` runs a **resilient launcher** (`~/.mic-bridge-launch.sh`) that:
1. waits for the Aggregate Device to appear (handles the **boot race** — see below),
2. detects the aggregate's current Pd device index via `pd -listdev`,
3. `exec`s Pd with that index and the patch.
KeepAlive restarts it; the launcher re-detects on every start.

**Two fragilities the prototype has (and `xlrbridge` must eliminate):**
1. **Index shift.** Pd selects devices by numeric index; the index of the Aggregate shifts whenever any device joins/leaves the list (e.g. quitting an app that provided a virtual device). → `xlrbridge` matches by **UID**, which never shifts.
2. **Boot race.** After reboot, Pd launched before the Aggregate was enumerated, failed to open audio, and **kept running idle without crashing** — so launchd's KeepAlive did not recover it. The launcher works around this by waiting. → `xlrbridge`'s `run` command must wait for the required devices (by UID) before opening, and must treat "audio failed to open" as a retry condition, not a silent idle state.

**Gain math:** digital gain factor = `10^(dB/20)`. Current prototype = +2 dB = `1.25892541`. Measured signal peak at +2 dB ≈ −6 dBFS; the clipping ceiling at the prototype's levels is roughly unity (0 dB) — do not push digital gain much past that. For more level, raise the interface's **analog** gain (better SNR; digital gain amplifies noise equally).

**Validation methodology (reused by `xlrbridge`'s tests):**
- **Dropout test:** record the virtual device while a *continuous* sound plays (a sustained hum/tone, NOT speech with pauses, so silence == real dropout). Run `silencedetect=noise=-50dB:d=0.04`; count `silence_start`. 0 = clean. The prototype scores 0; ffmpeg scored 9–12.
- **Quality:** `astats` for peak level (headroom/clipping), DC offset (~0), dynamic range, flat factor / peak count (clipping indicator). Prototype measured: peak −6 dBFS, DC 0.00007, dynamic range 90 dB, flat factor 0, 0 dropouts → clean for any consumer (OBS/Meet/recording, not just Discord).
- **Liveness:** record the virtual device 2 s; mean_volume above −91 dBFS = stream is flowing.

---

## 3. The solution: `xlrbridge` design

### 3.1 Principles
- **Native, dependency-light.** One small C binary linking CoreAudio + AudioToolbox. No Pd, no ffmpeg at runtime.
- **One external dependency:** BlackHole 2ch (the virtual device the consuming app reads). Writing our own virtual driver (an AudioServerPlugIn like BlackHole) is explicitly **out of scope for v1** — it is a whole signing/notarization/system-extension sub-project.
- **Robust by construction:** match devices by **UID**; create the Aggregate **programmatically** (no GUI); make it **private** so it doesn't clutter the user's device list; wait for devices before opening.
- **YAGNI:** v1 = one interface, choose input channel(s), mono→dual or stereo, one gain value, route to BlackHole. No multi-source, no multi-target, no DSP/plugins, no Linux.

### 3.2 Architecture / components
1. **Device layer** (`audio_devices.c`): enumerate audio devices via `kAudioHardwarePropertyDevices`; for each, read UID (`kAudioDevicePropertyDeviceUID`), name, input/output channel counts (`kAudioDevicePropertyStreamConfiguration`). Detect BlackHole (by name match "BlackHole" and/or UID). Resolve a device by UID at runtime.
2. **Aggregate layer** (`aggregate.c`): create/destroy a **private** Aggregate Device via `AudioHardwareCreateAggregateDevice(CFDictionary, &aggID)` / `AudioHardwareDestroyAggregateDevice`. Composition dictionary keys:
   - `kAudioAggregateDeviceUIDKey` — a stable UID we choose (e.g. `dev.xlrbridge.aggregate`).
   - `kAudioAggregateDeviceNameKey` — e.g. "xlrbridge engine".
   - `kAudioAggregateDeviceSubDeviceListKey` — array of `{ kAudioSubDeviceUIDKey: <interface UID> }, { kAudioSubDeviceUIDKey: <blackhole UID> }`.
   - `kAudioAggregateDeviceMasterSubDeviceKey` — the **interface** UID (master clock).
   - On the BlackHole sub-device entry: `kAudioSubDeviceDriftCompensationKey = 1` (drift-correct the non-master).
   - `kAudioAggregateDeviceIsPrivateKey = 1` — private (not visible/selectable system-wide; only our process uses it).
   - Set sample rate to a common value (48000) and confirm both sub-devices support it.
   - Compute the channel offsets of each sub-device within the aggregate (don't assume — read the aggregate's stream config; sub-device order in the list defines channel order, but verify).
3. **Routing engine** (`engine.c`): `AudioDeviceCreateIOProcID(aggID, ioproc, ctx, &procID)`, `AudioDeviceStart`. In the IOProc: for each frame, read input buffer at `srcChannel` (the chosen interface input channel), multiply by `gain`, write to the BlackHole output channels (`dstChannelL`, `dstChannelR`). Handle buffer/stride correctly (interleaved vs per-buffer `AudioBufferList`). Mono source → write to both dst channels (dual-mono); stereo source → map two src channels.
4. **Boot/readiness wait:** before creating the IOProc, poll until both the interface UID and BlackHole UID resolve to live devices; if not present, sleep and retry (bounded loop, then exit non-zero so launchd retries). Also register a listener on `kAudioHardwarePropertyDevices` to react to hot-plug (optional v1.1).
5. **Config** (`config.c`): JSON at `~/.config/xlrbridge/config.json` — `{ interface_uid, input_channels: [n] or [l,r], gain_db, virtual_uid }`. (Use a tiny JSON lib or hand-roll; avoid heavy deps.)
6. **launchd integration** (`service.c`): write `~/Library/LaunchAgents/dev.xlrbridge.plist` running `xlrbridge run`, `RunAtLoad`, `KeepAlive`, `StandardError/OutPath` to a log; load/unload via `launchctl` (or the `launchd` API). KeepAlive + the engine's own readiness-wait covers the boot race.
7. **CLI** (`main.c`): subcommand dispatch.

### 3.3 CLI surface
| Command | Behavior |
|---|---|
| `xlrbridge devices` | List interfaces with input channels + UIDs; flag whether BlackHole is installed. |
| `xlrbridge setup` | Interactive: choose interface, choose input channel(s), set gain; verify/instruct BlackHole install; create the aggregate; write config; install + load the launchd agent; verify signal. |
| `xlrbridge run` | The daemon launchd runs: read config, wait for devices (by UID), ensure aggregate exists, start IOProc, block. On device loss, stop cleanly and exit non-zero (KeepAlive restarts). |
| `xlrbridge status` | Is the agent loaded + engine running? Optionally do a 2 s liveness capture of BlackHole and report level. |
| `xlrbridge fix` | Re-resolve devices, recreate the aggregate if missing, reload the agent. (Rarely needed thanks to UID matching — keep it as a panic button.) |
| `xlrbridge gain <dB>` | Update gain in config; signal the running engine to reload (or restart it). |
| `xlrbridge uninstall` | Unload+remove the agent, destroy the aggregate, optionally remove config. |

### 3.4 Distribution
- GitHub repo (MIT license).
- `Makefile`: `clang -framework CoreAudio -framework AudioToolbox -framework CoreFoundation`.
- Homebrew tap formula building from source; `brew install <tap>/xlrbridge`.
- Strong `README.md`: the problem story (§1), a 30-second "is this you?" symptom check, install, `xlrbridge setup`, troubleshooting. The diagnostic narrative is the SEO/empathy hook — people Googling "Discord mic choppy audio interface mac" should land here.
- BlackHole is a dependency: the formula can declare it / `setup` can `brew install blackhole-2ch` or link to it. Note BlackHole is GPL but we only *depend on / talk to* it (separate process/driver), so `xlrbridge` can be MIT.

---

## 4. Implementation plan (phased, for the implementing agent)

Each phase is independently testable. Commit per phase. Build the validation harness early (Phase 0) so every later phase is verifiable.

**Phase 0 — Scaffold + validation harness.**
- Repo, `Makefile`, MIT `LICENSE`, `README` skeleton, `.gitignore`.
- A `tools/measure.sh` that records BlackHole and reports dropouts (`silencedetect`) + `astats` (peak/DC/dropouts). This is your oracle for "is the routing clean." (Port from the prototype's test commands.)
- Acceptance: `make` builds a stub binary; `tools/measure.sh` runs against the existing prototype and reports 0 dropouts.

**Phase 1 — Device enumeration (`devices`).**
- Enumerate devices; print name, UID, in/out channel counts; detect BlackHole.
- Acceptance: lists the user's interface + BlackHole with correct channel counts and stable UIDs.

**Phase 2 — Aggregate create/destroy.**
- `AudioHardwareCreateAggregateDevice` (private, master=interface, drift-comp on BlackHole) from two UIDs; `AudioHardwareDestroyAggregateDevice`.
- Determine channel offsets of each sub-device within the aggregate programmatically.
- Acceptance: aggregate appears to the process (private), correct sample rate, destroyable cleanly; no leak of aggregates across runs (reuse by UID).

**Phase 3 — Routing engine (`run`, foreground first).**
- IOProc copying interface input channel → BlackHole output channels × gain.
- Readiness wait (poll UIDs) before start.
- Acceptance: run in foreground; `tools/measure.sh` reports **0 dropouts** on a continuous tone, peak below 0 dBFS, DC ≈ 0 — matching the Pd prototype. THIS IS THE CORE MILESTONE.

**Phase 4 — Config + `setup`.**
- JSON config read/write; interactive `setup` flow; verify BlackHole present (instruct install if not).
- Acceptance: `setup` from scratch produces a working config + running engine; `run` reads config.

**Phase 5 — launchd service (`status`, `fix`, `uninstall`).**
- Install/load/unload the agent; KeepAlive; engine survives logout/login and **reboot** (test the boot race explicitly: reboot, confirm audio flows without manual intervention).
- Acceptance: after reboot, `status` shows running + signal flowing; `silencedetect` clean.

**Phase 6 — `gain`, polish, packaging.**
- `gain <dB>` live update; clamp/ warn near clipping.
- Homebrew formula; README with full problem story; example output; troubleshooting (BlackHole missing, interface unplugged, permissions/mic TCC).
- Acceptance: a fresh Mac can `brew install` → `xlrbridge setup` → working clean mic in Discord.

**Cross-cutting concerns to handle:**
- **TCC / microphone permission:** opening an input device may require microphone permission for the running process; `run` under launchd must have it. Document/handle the prompt.
- **Sample-rate negotiation:** confirm both sub-devices support the chosen rate; fall back gracefully.
- **Hot-plug:** v1 can rely on launchd restart + readiness wait; v1.1 can add a device-list listener for instant re-attach.
- **Idempotency:** re-running `setup` should reuse the aggregate (match by our chosen UID) rather than pile up duplicates.
- **Clean teardown:** destroy the aggregate on uninstall; don't orphan private aggregates.

---

## 5. Non-goals (v1)
- Own virtual audio driver (keep BlackHole).
- Linux (different stack — PipeWire/PulseAudio; the routing there is near-trivial and is a separate project, not a shared codebase).
- Multiple simultaneous sources / multiple target virtual devices.
- DSP/plugins (EQ, gate, compressor, VST/AU hosting). The prototype notes these are achievable later (native CoreAudio DSP in the IOProc, or hosting via a separate path), but they are out of v1.
- GUI.

---

## 6. References
- BlackHole (virtual audio driver): https://github.com/ExistentialAudio/BlackHole — passthrough, no gain/DSP, unity by design.
- Apple CoreAudio: `AudioHardwareCreateAggregateDevice`, `kAudioAggregate*`/`kAudioSubDevice*` keys, `AudioDeviceCreateIOProcID`, `kAudioDevicePropertyDeviceUID`, `kAudioDevicePropertyStreamConfiguration`.
- Pure Data (prototype engine): https://puredata.info — `brew install --cask pd`.
- Prototype artifacts: see Appendix (they encode the exact working routing + channel map + launchd behavior to port).

---

## 7. Appendix — exact working prototype artifacts

These are the files that currently work on the originating machine (E2x2). Port their *behavior*; the UIDs/indices are machine-specific.

### 7.1 Pure Data patch — `~/.mic-bridge.pd`
```
#N canvas 0 0 460 320 12;
#X obj 40 50 adc~ 1;
#X obj 40 210 dac~ 7 8;
#X obj 300 40 loadbang;
#X msg 300 90 \; pd dsp 1;
#X obj 40 130 *~ 1.25892541;
#X text 175 132 <- gain +2 dB (1.25892541 = 10^(2/20));
#X connect 0 0 4 0;
#X connect 2 0 3 0;
#X connect 4 0 1 0;
#X connect 4 0 1 1;
```
Reads aggregate input ch1 (Analogue 1), applies +2 dB, writes aggregate outputs 7 & 8 (BlackHole). DSP auto-on.

### 7.2 Resilient launcher — `~/.mic-bridge-launch.sh`
Waits for the Aggregate to appear, detects its Pd device index (input + output, parsed from `pd -listdev`), then `exec`s Pd. Exits non-zero if the aggregate never appears (launchd retries). This shell-level index detection is exactly what `xlrbridge` replaces with UID matching.
```bash
#!/bin/bash
PATCH="$HOME/.mic-bridge.pd"
PD=$(ls -d /Applications/Pd-*.app/Contents/Resources/bin/pd 2>/dev/null | sort -V | tail -1)
[ -z "${PD:-}" ] && { echo "$(date): Pd not found"; exit 1; }
for try in $(seq 1 15); do
  tmp=$(mktemp)
  "$PD" -nogui -stderr -listdev </dev/null >"$tmp" 2>&1 &
  pid=$!; sleep 3; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  IN=$(awk '/audio input devices:/{s=1;next} /audio output devices:/{s=0} s && /Aggregate Device/{gsub(/\..*/,"",$1);print $1;exit}' "$tmp")
  OUT=$(awk '/audio output devices:/{s=1;next} s && /Aggregate Device/{gsub(/\..*/,"",$1);print $1;exit}' "$tmp")
  rm -f "$tmp"
  if [ -n "$IN" ] && [ -n "$OUT" ]; then
    exec "$PD" -nogui -stderr -r 48000 -inchannels 8 -outchannels 8 \
      -audioindev "$IN" -audiooutdev "$OUT" -open "$PATCH"
  fi
  sleep 4
done
exit 1
```

### 7.3 LaunchAgent — `~/Library/LaunchAgents/com.scoobert.micbridge.plist`
Runs `/bin/bash ~/.mic-bridge-launch.sh`, `RunAtLoad`, `KeepAlive`, `ThrottleInterval 10`, logs to `/tmp/micbridge.log`. `xlrbridge` ships the equivalent as `dev.xlrbridge.plist` running `xlrbridge run`.

### 7.4 Aggregate Device (manual, via Audio MIDI Setup)
`[E2x2 + BlackHole 2ch]`, 48 kHz, Clock Source = E2x2, Drift Correction ON for BlackHole, E2x2 listed first → 8 in / 8 out with the channel map in §2. `xlrbridge` creates this programmatically and privately.

### 7.5 Measurement commands (the test oracle)
```bash
# dropouts on a CONTINUOUS tone (record while a sustained hum plays):
ffmpeg -hide_banner -i sample.wav -af "silencedetect=noise=-50dB:d=0.04" -f null /dev/null 2>&1 | grep -c silence_start   # 0 = clean

# quality:
ffmpeg -hide_banner -i sample.wav -af astats=metadata=1 -f null /dev/null 2>&1 \
  | grep -iE 'Peak level|DC offset|Dynamic range|Flat factor|Peak count'

# liveness (record the virtual device 2s; > -91 dBFS mean = flowing):
ffmpeg -hide_banner -loglevel error -f avfoundation -i ":BlackHole 2ch" -t 2 -y /tmp/c.wav && \
ffmpeg -hide_banner -i /tmp/c.wav -af volumedetect -f null /dev/null 2>&1 | grep mean_volume
```
ffmpeg is used ONLY for testing here — never for the runtime routing (it drops audio; see §1.5).
