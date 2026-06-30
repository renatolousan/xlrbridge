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

## Status
See `.superpowers/sdd/progress.md` for the live phase ledger.
