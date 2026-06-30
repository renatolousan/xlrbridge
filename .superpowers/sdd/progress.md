# xlrbridge v1 — progress ledger (branch feat/v1)

Plan: HANDOFF.md §4 (Phases 0–6). Coordinator = main session; implementers = Opus subagents.
Rule: always keep one implementer in flight so completion notifications re-drive the loop — never end a turn idle until v1 is done.

- Phase 0: complete (ff4c56c) — scaffold + Makefile + measure.sh.
- Phase 1: complete (94fa21f) — `devices`.
- Phase 2: complete (648bbaf) — aggregate layer + `_aggtest`.
- Phase 3: complete (03f5c67) — routing engine `run`, 0 dropouts proven, RT-safe.
- Phase 4: complete (039719a) — config + setup + run-reads-config + --dry-run.
- Phase 5: complete (5ee327e) — launchd service (service.{c,h}), setup auto-install (gated), status/fix/uninstall. Cutover = FALLBACK (Pd left live; engine starts under launchd w/ no TCC denial, but 0-dropout validation blocked by an avfoundation capture wedge — needs user `sudo killall coreaudiod`). make compiles clean (exit 0).
- Phase 6: complete — `gain <dB>` (updates config; reloads agent if loaded, else config-only; validates range/clipping), engine.c defensive output zeroing, service.h `<stddef.h>`, real README, Formula/xlrbridge.rb, Makefile install/uninstall, version 1.0.0. make clean+build exit 0, no warnings. Verified by build + code review only (capture path wedged → no audio commands run). gain config round-trip tested (other fields preserved; restored to +2 dB).

## Open findings → folded into Phase 6 (DONE)
- DONE: engine.c IOProc now zeroes ALL output buffers (memset, RT-safe) before writing the two BlackHole lanes — no stale buffer noise leaks to interface outputs.
- DONE: src/service.h already carried `#include <stddef.h>` (compiles standalone, verified with `clang -fsyntax-only`).

## Blocked-on-user (note for Renato)
- CUTOVER to xlrbridge as the live bridge is NOT done. The avfoundation capture path is wedged (from kill -9 of hung ffmpeg during Phase 5 testing); clears with `sudo killall coreaudiod`. After that: unload Pd, `xlrbridge setup` (installs dev.xlrbridge agent), run the loopback-tone 0-dropout validation, and if clean the cutover is complete. Until then, Pd remains the live bridge (HAL path, unaffected by the avfoundation wedge → Discord mic works).

## Design refinements
- Aggregate PRIVATE, created/destroyed by `run`. Channel layout (E2x2): iface inputs offset 0; BlackHole outputs offset 6.
- launchd label: dev.xlrbridge (xlrbridge); com.scoobert.micbridge (Pd prototype, fallback, plist kept on disk).
