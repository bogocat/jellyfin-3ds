# Issue 7: Seek breaks video image after 2–3 seeks (audio keeps playing)

> **FILED as https://github.com/bogocat/jellyfin-3ds/issues/7** (2026-06-11, with updated root-cause analysis: duplicate PlaySessionId on seek — fixed in PR #16 — plus missing transcode-job cleanup)
> **Suggested title:** Seek breaks video image after 2–3 seeks (audio keeps playing)
> **Suggested labels:** `area:video`, `bug`
> **Hardware repro only** — needs real 3DS, not Citra (Citra networking is broken)

---

## Lead

Reported by a v1.0.0 user on Reddit: when watching a TV episode, the first 1–2 seeks (skip ±30s) work correctly, but after that the video image goes black while audio continues uninterrupted. Backing out to the browse view and re-opening the same episode restores video. This is reproducible on real hardware, only on TV episodes per the report (not yet confirmed for movies).

## Scope

- [ ] Reproduce on real 3DS hardware with a TV episode library item
- [ ] Identify the state that is not being cleaned up on the 2nd–3rd `video_player_play` call (likely MVD decoder, frame queue, or one of the worker threads)
- [ ] Add explicit `mvd_exit()` / state-flag flush on the in-place restart path
- [ ] Verify on at least 5 consecutive seeks without backing out

## Out of scope

- Adding seeking to video (already shipped in v1.0.0; this is a regression-fix on the existing path)
- Changing the seek offset unit (ticks vs seconds)
- New UI for the seek bar (existing ±30s on L/R is fine)

## Acceptance criteria

- [ ] 10 consecutive seeks in a row on a TV episode play through without black-image failure
- [ ] Audio and video remain in sync across all 10 seeks (no PTS drift)
- [ ] Memory and thread state are clean after each seek (no leaks — log_write shows frame queue size returns to baseline)
- [ ] User-reported workaround (back-out + re-open) is no longer needed

## Likely shape

Suspect: `video_player_play()` in `src/video/video_player.c` joins the network/decode/convert threads and re-initializes state, but the MVD hardware state may not be symmetric to init. First seek works because the MVD was idle; subsequent seeks reuse a partially-reset MVD. Capture `log_write` output during a failing seek to confirm. Look for a missing `mvd_exit()` call or an un-flushed texture between stops.

## References

- Reddit thread (incoming; URL TBD)
- v1.0.0 release: commit `37e80d5` (subtitle burn-in + seek release)
- Seek implementation: `src/video/video_player.c:855` (`video_player_play` with `seek_offset_ticks`)
- MVD init: `src/video/ffmpeg_demux.c`
- M3 (MiniMax) parallel issue review: not applicable, this is C-only
