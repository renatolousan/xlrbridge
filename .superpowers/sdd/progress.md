# xlrbridge v1 — progress ledger (branch feat/v1)

Plan: HANDOFF.md §4 (Phases 0–6). Coordinator = main session; implementers = Opus subagents.
Rule: always keep one implementer in flight so completion notifications re-drive the loop — never end a turn idle until v1 is done.

- Phase 0: complete (ff4c56c) — scaffold + Makefile + measure.sh.
- Phase 1: complete (94fa21f) — `devices` (UID enum, BlackHole detect, exit codes).
- Phase 2: complete (648bbaf) — aggregate layer + `_aggtest` (private, offsets from real layout, idempotent). Verified.
- Phase 3: complete (03f5c67) — routing engine `run` + IOProc. CORE milestone proven: 0 dropouts on continuous tone, RT-safe IOProc, aggregate destroyed on all paths, readiness wait. Code-reviewed by coordinator. Pd prototype restored & verified alive.
- Phase 4: complete — config persistence (`src/config.{c,h}`, flat JSON at ~/.config/xlrbridge/config.json, hand-rolled tolerant parser/serializer, no deps), `setup` command (interactive + non-interactive flags `--interface-uid/--in-channel/--gain-db/--blackhole-uid/--yes`, detect+instruct BlackHole, never auto-installs), `run` now loads config (flags > config > defaults), `run --dry-run` (resolve+validate+print plan, exit 0, no device grab — non-disruptive). Version 0.4.0-phase4. Tested against live E2x2 + BlackHole without disturbing the Pd bridge.
- Phase 5: pending (launchd service, status/fix/uninstall, boot race, CUTOVER from Pd prototype to xlrbridge).
- Phase 6: pending (gain, polish, packaging, README).

## Open findings (for final review)
- MINOR (Phase 3): IOProc writes only BlackHole output channels; doesn't zero the interface's other output channels. Prototype did the same with no issue, but defensively zeroing unused outputs is best practice. Address in a later phase or final review.

## Design refinements
- Aggregate PRIVATE, created/destroyed by `run` (not setup). setup only writes config + (Phase 5) installs agent.
- Channel layout (E2x2): iface inputs at offset 0; BlackHole outputs at offset 6. Engine: input[in_channel]→output[6],[7] × gain.

## IMPORTANT — keep the user's mic bridge alive
Pd prototype (LaunchAgent com.scoobert.micbridge) is the user's LIVE Discord fix. Tests that start the xlrbridge engine must stop Pd first (contention) then restart it (`bash ~/.claude/skills/fix-mic-bridge/repair.sh`). CUTOVER to the xlrbridge binary happens in Phase 5 once fully validated. Prefer non-disruptive tests (e.g. `run --dry-run`) where possible.
