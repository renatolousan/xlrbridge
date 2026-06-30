# xlrbridge v1 — progress ledger (branch feat/v1)

Plan: HANDOFF.md §4 (Phases 0–6). Coordinator = main session; implementers = Opus subagents.
Rule: always keep one implementer in flight so completion notifications re-drive the loop — never end a turn idle until v1 is done.

- Phase 0: complete (ff4c56c) — scaffold + Makefile + measure.sh.
- Phase 1: complete (94fa21f) — `devices`.
- Phase 2: complete (648bbaf) — aggregate layer + `_aggtest`.
- Phase 3: complete (03f5c67) — routing engine `run`, IOProc, 0 dropouts proven, RT-safe. Pd restored.
- Phase 4: complete (039719a) — config + `setup` + `run` reads config + `run --dry-run`. Verified non-disruptively; Pd bridge untouched.
- Phase 5: complete — launchd service (src/service.{c,h}), setup auto-installs+loads dev.xlrbridge agent (gated: refuses while Pd loaded; --no-service skips), status/fix/uninstall implemented, version 0.5.0-phase5. CUTOVER OUTCOME = **FALLBACK (Pd left live)**, see below.
- Phase 6: pending (gain, polish, packaging, README).

## Open findings (for final review)
- MINOR (Phase 3): IOProc doesn't zero the interface's non-BlackHole output channels. Prototype did the same with no issue. Defensive zeroing is best practice.
- Phase 5 cutover = FALLBACK. The launchd-run xlrbridge engine DID start cleanly under launchd (created its private aggregate, no TCC denial in /tmp/xlrbridge.log: it logged "aggregate id=185 ... routing in ch 2 -> BlackHole out 6,7" and ran with no AudioDeviceStart failure once no other client held BlackHole). BUT the continuous-tone validation could NOT be measured: during testing, force-killing (kill -9) hung ffmpeg avfoundation capture clients wedged the CoreAudio *capture* path system-wide — afterwards even a plain `ffmpeg -f avfoundation -i :BlackHole 2ch`/`:E2x2` capture hangs (rc=137) with NO bridge running. Without a working capture, the mandated "0 dropouts" SUCCESS evidence was unobtainable, so per CUTOVER SAFETY we did NOT cut over.
- Action left for the user: the avfoundation capture wedge clears with a coreaudiod restart (`sudo killall coreaudiod`) — not run here (no sudo / would disrupt all audio). The wedge is in the avfoundation record path; Discord uses the CoreAudio HAL input path (likely separate), and Pd is writing BlackHole normally, so Discord mic may well be fine — but unverified.
- TCC finding: NO microphone-permission denial was observed for the launchd-run engine. The engine opened the aggregate (whose master sub-device is the E2x2 input) under launchd without a TCC error. The one failure seen (OSStatus 268451843) was device contention from the hung capture client, not permission. (Caveat: not measured end-to-end due to the capture wedge.)
- `status`/liveness hardened: the avfoundation capture in `xlrbridge status` is now hard-killed after 8 s so a wedged capture path can never hang the command; it reports "capture FAILED" instead.

## Phase 5 final state (FALLBACK)
- LOADED + running: com.scoobert.micbridge (Pd, the live bridge).
- dev.xlrbridge: NOT loaded; plist left on disk (~/Library/LaunchAgents/dev.xlrbridge.plist) as the documented fallback (proven to start under launchd).
- config.json restored to real mic: in_channel 0, gain_db 2.
- Never had both agents loaded simultaneously.

## Design refinements
- Aggregate PRIVATE, created/destroyed by `run`. setup writes config; Phase 5 setup also installs the launchd agent (label `dev.xlrbridge`).
- Channel layout (E2x2): iface inputs offset 0; BlackHole outputs offset 6.

## CUTOVER SAFETY (Phase 5) — the user must wake to a WORKING bridge
Pd prototype (LaunchAgent com.scoobert.micbridge) is the LIVE bridge. Phase 5 cutover: stop Pd, install+load the xlrbridge agent, then VALIDATE the launchd-run engine actually gets signal (continuous-tone test on a loopback channel — this also proves macOS mic/TCC permission works under launchd, a known gotcha).
- If validated → leave xlrbridge LIVE (config back to channel 0 = real mic); keep Pd's plist on disk (unloaded) as documented fallback.
- If it fails (TCC-denied silence / any error) → DO NOT cut over: reload Pd (`bash ~/.claude/skills/fix-mic-bridge/repair.sh`), leave xlrbridge agent unloaded, document the issue for the user.
