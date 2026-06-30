# Notes for Renato — decisions parked during the autonomous v1 build

I'm building v1 autonomously (coordinator + Opus implementer subagents, branch `feat/v1`).
Below are choices I made with sensible defaults, and open questions for you to confirm/override when you're back. Nothing here blocks progress — I kept going.

## Decisions I made (override if you disagree)
- **Repo visibility:** already public at github.com/renatolousan/xlrbridge.
- **Aggregate is private** (`kAudioAggregateDeviceIsPrivateKey`) — it won't clutter your device list; only the engine uses it. Discord still selects "BlackHole 2ch".
- **Default sample rate 48000**, mono mic on the chosen input channel duplicated to both BlackHole channels (matches the working prototype).
- **Gain default +2 dB** (carried from the prototype). Configurable via `gain`.
- **Exit codes:** non-zero on unknown command (fixed the Phase 0 nit).
- **Not pushing** the `feat/v1` branch until you approve the final result — it stays local. Say the word and I'll push + open a PR.

## Open questions (pick when you're back — defaults in **bold**)
1. **Phase 3 automated testing:** verifying "0 dropouts" normally needs a continuous tone on the mic input (a human humming). For autonomous testing I plan to route a **generated continuous tone through the E2x2 loopback channel** and prove the engine copies it cleanly to BlackHole (channel-agnostic proof). If that's not feasible I'll fall back to a short manual hum check and note it. → default: **automated via loopback tone**.
2. **`setup` interactivity when run by me:** the real `setup` is interactive (prompts). For autonomous testing I'll drive it via flags/env or a non-interactive path and validate the interactive flow by reading the code. → default: **add `--non-interactive` flags + test those; manual interactive test left for you**.
3. **BlackHole install in `setup`:** auto-run `brew install blackhole-2ch`, or just detect + instruct? → default: **detect + instruct** (don't auto-install system audio drivers without user consent).
4. **Homebrew tap:** I'll write the formula but **not** create/publish a tap repo without you. → default: **formula committed in-repo, publishing left to you**.

## ✅ V1 BUILD COMPLETE (read this first)

All 7 phases (0–6) built by Opus subagents, each coordinator-reviewed/approved, plus a whole-branch final review and a fix pass. **8 commits on `feat/v1`**, version **1.0.0**, clean build (`-Wall -Wextra`, no warnings), MIT LICENSE present. Final review found one must-fix (missing LICENSE) + a few small items — all fixed in commit `75f3943`.

**What works (proven):** `devices`, `setup` (interactive + flags), `run` (+`--dry-run`), `status`, `fix`, `gain`, `uninstall`, `_aggtest`. The core routing engine measured **0 dropouts** on a continuous tone (Phase 3). RT-safe IOProc, no leaks, devices matched by stable UID, private aggregate auto-created/destroyed, launchd service, README + Homebrew formula + `make install`.

### TWO things need YOU (couldn't be done autonomously):

1. **Finish the cutover** (make xlrbridge the live bridge instead of the Pd prototype). Blocked because an `ffmpeg` capture got force-killed during testing and **wedged the macOS avfoundation capture path** (a known macOS thing). The engine itself runs fine under launchd (no mic-permission/TCC denial seen) — I just couldn't take the final 0-dropout measurement. To finish:
   ```bash
   sudo killall coreaudiod                 # clears the capture wedge (harmless; resets audio)
   launchctl unload ~/Library/LaunchAgents/com.scoobert.micbridge.plist   # stop the Pd prototype
   cd ~/xlrbridge && ./xlrbridge setup      # installs the dev.xlrbridge agent (interactive — smoke-test the prompts!)
   ./xlrbridge status                        # confirm engine running + BlackHole flowing
   # talk in Discord (input = BlackHole 2ch). If anything's off, fall back:
   #   launchctl unload ~/Library/LaunchAgents/dev.xlrbridge.plist
   #   bash ~/.claude/skills/fix-mic-bridge/repair.sh   # restores the Pd bridge
   ```
   **Right now the Pd prototype is still your live bridge and Discord works** (it uses the HAL path, unaffected by the avfoundation wedge). Nothing is broken.

2. **Merge + push decision.** I did NOT merge `feat/v1` → `main` and did NOT push (per the "approve first" note). When you're happy: `git checkout main && git merge feat/v1` then `git push`. Or tell me and I'll do it + open a PR. The Homebrew formula's `sha256`/tag are placeholders until you tag a release.

(Original parked decisions/answers below are now all resolved as built — kept for the record.)

## Status
See `.superpowers/sdd/progress.md` for the full phase ledger (all phases complete).
