# xlrbridge

> *fixing Discord's problem with handling xlr mics with fancy interfaces*

A small native macOS CLI (C + CoreAudio) that routes one input channel of a
pro/XLR audio interface into a clean virtual device (BlackHole), so Discord (and
similar apps) stop **chopping** your audio and **auto-limiting** your input
level.

## Is this you?

- You use an XLR mic through an audio interface (Topping, Focusrite, MOTU,
  PreSonus…).
- In Discord your voice **stutters/chops** every fraction of a second, or the
  input level gets **pulled down** the moment you start talking (AGC pumping).
- But the *same* mic records perfectly in QuickTime or your DAW, and Google Meet
  sounds fine on the same machine and network.
- It chops even in Discord's **in-app mic test** (no network involved) and in the
  browser.

If that's you, it isn't your mic, your interface, or your network. It's a macOS
quirk — and `xlrbridge` fixes it.

## How it works

When an app opens a multichannel USB interface **directly**, macOS applies a
voice-processing path (AGC + echo/noise suppression) that limits the level and
glitches on the interface's stream. The fix is to never let the app open the
interface. Instead:

1. `xlrbridge` creates a **private Aggregate Device** combining your interface
   and **BlackHole 2ch**, in a single CoreAudio clock domain with drift
   correction. One clock domain is the whole trick — copying samples between two
   free-running clocks drips and drops audio in chunks.
2. A tiny real-time **IOProc** copies your chosen interface input channel
   (× a digital gain) into BlackHole's outputs, which loop back to BlackHole's
   inputs (dual-mono).
3. Discord opens **BlackHole 2ch** as its mic — a clean 2-channel device — so
   macOS never applies the interface voice-processing.

A `launchd` agent runs the engine at login and keeps it alive across reboots,
matching devices by **stable UID** (not index), so it survives replugging and
device-list reordering.

**Why not ffmpeg?** ffmpeg is a file/stream transcoder; its real-time sink
underruns and drops audio (measured: 9–12 dropouts on an 8 s tone). A native
CoreAudio IOProc inside the aggregate drops **zero**. See
[`HANDOFF.md`](./HANDOFF.md) §1.5 for the full teardown.

## Install

### Homebrew (tap)

```bash
brew install blackhole-2ch          # the virtual device (required, see below)
brew install renatolousan/xlrbridge/xlrbridge
```

(The formula lives in [`Formula/xlrbridge.rb`](./Formula/xlrbridge.rb).)

### Build from source

```bash
git clone https://github.com/renatolousan/xlrbridge.git
cd xlrbridge
make
sudo make install            # installs to /usr/local/bin (override with PREFIX=)
# or just run ./xlrbridge in place
```

`make install` honours `PREFIX` (default `/usr/local`); for Apple-silicon
Homebrew prefixes use `make install PREFIX=/opt/homebrew`. `make uninstall`
removes the binary.

### BlackHole dependency

`xlrbridge` depends on **BlackHole 2ch** — the clean virtual audio device Discord
reads. It is a free, open-source virtual driver:

```bash
brew install blackhole-2ch
```

or download it from <https://existential.audio/blackhole/>. `xlrbridge` only
*talks to* BlackHole as a separate driver, so `xlrbridge` itself is MIT even
though BlackHole is GPL.

## Usage

```text
xlrbridge <command> [args]
```

| Command | What it does |
|---|---|
| `xlrbridge setup` | Interactive: pick your interface, the mic's input channel, and the gain; verify BlackHole is installed; write the config; install + load the `dev.xlrbridge` LaunchAgent so the engine runs at login. Non-interactive flags available (`--interface-uid`, `--in-channel`, `--gain-db`, `--blackhole-uid`, `--yes`, `--no-service`). |
| `xlrbridge run` | The daemon `launchd` runs: read config, wait for the devices (by UID), create the private aggregate, start the routing IOProc, block. `--dry-run` validates + prints the plan without grabbing any device or starting audio. |
| `xlrbridge status` | Report whether the agent is loaded + the engine is running, do a short BlackHole liveness check, and note whether the Pd prototype bridge is also loaded. |
| `xlrbridge fix` | Panic button: re-resolve devices and reload the agent (unload + load). Rarely needed thanks to UID matching. |
| `xlrbridge gain <dB>` | Set the digital gain (e.g. `xlrbridge gain 2`), persist it to config, and apply it — reloads the agent if it's loaded; otherwise it takes effect on next start. Warns above 0 dB (clipping risk); rejects absurd values. Prefer raising the interface's **analog** gain for more level — better SNR. |
| `xlrbridge uninstall` | Unload + remove the agent. The engine destroys its private aggregate on stop; config is left in place. |
| `xlrbridge devices` | List all audio devices with input/output channel counts and UIDs, and flag whether BlackHole is installed. |

Typical first run:

```bash
xlrbridge devices     # confirm your interface + BlackHole are present
xlrbridge setup       # pick channel/gain, install the login service
xlrbridge status      # confirm the engine is running and signal is flowing
# then in Discord, set the input device to "BlackHole 2ch"
```

## Troubleshooting

- **Microphone permission (TCC).** The first time the engine opens the
  interface, macOS asks for microphone permission. Approve it, or grant it under
  **System Settings → Privacy & Security → Microphone**. Under `launchd`, if the
  prompt never appears and `status` shows no signal, run `xlrbridge run` once in
  a terminal to trigger the prompt, then reload (`xlrbridge fix`).
- **Device replug / reorder.** `xlrbridge` matches by UID, so unplugging and
  replugging the interface is fine — the engine waits for it and re-attaches.
  If it doesn't recover, `xlrbridge fix` reloads the agent.
- **No signal in Discord.** Confirm Discord's input device is set to
  **BlackHole 2ch** (not the interface). Run `xlrbridge status` to see whether
  the engine is running and whether BlackHole has signal.
- **Capture acts up / everything is wedged.** If macOS audio capture gets stuck
  (a hung capture client can wedge CoreAudio's `avfoundation` path), reset the
  audio daemon:

  ```bash
  sudo killall coreaudiod
  ```

  This restarts CoreAudio; the engine's `launchd` agent restarts automatically.

For the full diagnosis, design rationale, channel map, and the measurement
methodology, see [`HANDOFF.md`](./HANDOFF.md).

## Platform & non-goals (v1)

macOS only. v1 deliberately does **not** ship its own virtual driver (it uses
BlackHole), Linux support, multiple sources/targets, DSP/EQ/gate/compressor, or
a GUI. See [`HANDOFF.md`](./HANDOFF.md) §5.

## License

MIT.
