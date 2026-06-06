# Reddit Feedback Round 1 — Triage

> **Date:** 2026-06-05
> **Source:** Reddit user (thank you!), reporting on v1.0.0
> **Purpose:** Triage every actionable item from the message, link to the WIP work where it overlaps, and surface the open questions to the author

This doc triages the Reddit user's report into four buckets:

1. **Bugs** — file as GitHub issues (already drafted in `docs/issues/`)
2. **Already in the WIP** — confirm in the in-flight branch
3. **Easy extensions of the WIP** — small adds, can land in the same milestone
4. **Milestone-worthy features** — separate effort, may or may not happen

## Bugs → GitHub issues

| # | Bug | Status |
|---|-----|--------|
| 1 | Seek breaks video image after 2–3 seeks (audio keeps playing) | Drafted: `docs/issues/0007-seek-breaks-image-after-2-3-seeks.md` |
| 2 | "Home Videos and Photos" library: videos don't load | Drafted: `docs/issues/0008-home-videos-video-type-not-recognized.md` |

Both drafts follow the repo's `gh-issue-structure.md` convention (Scope / Out of scope / Acceptance criteria / References). They are written as ready-to-paste issue bodies — the user can file them via the web UI to preserve the serrattleballs pseudonym (gh CLI on this machine is authenticated as jakecelentano).

## Already in the WIP (just needs to land + be tested)

These are the good-news items. The WIP branch `wip/hsbs-3d-and-settings` already implements two of the suggestions, so the Reddit user's intuition matches the in-flight direction.

| Suggestion | Status in WIP | Notes |
|------------|---------------|-------|
| **Settings button on main menu** (change server / user without editing the config file) | **Done in WIP** — SELECT on the libraries view opens `VIEW_SETTINGS` (see `src/ui/ui.c:388-393`) | The settings screen has server URL, username, audio/video bitrate pickers, auto-advance toggle, logout, version, device ID. Needs CLAUDE.md and README updates to document the new SELECT binding |
| **Background fades to black during playback** | **Partly in WIP** — the new color palette has warmer darks; render path can clear the background more aggressively. Not explicitly a "fade" yet | Easy follow-up; 5–15 lines in `ui_render_now_playing()` to skip card backgrounds and clear both screens at the top of the render call |

## Easy extensions of the WIP (small, same milestone)

| Suggestion | Effort | Notes |
|------------|--------|-------|
| Fade to black on playback start | ~15 lines | Trivial; see above. Add as a small commit on top of the WIP branch |
| "Continue Watching" / "Resume" surfaced in the libraries view | ~30 lines + a new render pane | `jfin_get_resume()` already exists in `src/api/jellyfin.c`. Just needs a UI hook to call it and render results. Perfect v1.1 follow-up |

## Milestone-worthy (separate effort)

These are real features, but they're each a meaningful chunk of work. Worth doing, but should not be folded into the HSBS/settings PR.

| Suggestion | Effort | Notes |
|------------|--------|-------|
| **Cover art Netflix-style horizontal scroll** | 1–2 PRs of dedicated work | Requires JPEG decode on 3DS (libjpeg-turbo cross-compile or miniz + custom decoder), HTTP image fetch (libcurl is already linked), texture upload, and a redesign of `ui_render_browse()`. The yuckyman fork's `feat(ui): add cached preview art` is a starting point but uses a different pattern (small cached previews vs. full cover scroll). Best as a v1.2 milestone: "Cover Art Home Screen" |
| **L/R panes on the libraries view: Libraries / Next Up / Favorites** | 1 PR | High value (WiiFin users expect this). The 3 endpoints exist: `/Items/Resume` (already wrapped as `jfin_get_resume`), `/Shows/NextUp`, and `&Filters=IsFavorite`. L/R currently paginates inside a list; the remap is `if (current_view == VIEW_LIBRARIES) L/R = switch pane, else L/R = page`. Could combine with the cover-art milestone above |

## Reddit reply (for the user to send)

Short thank-you + acknowledgment of filed bugs + heads-up that the settings button is on the way. Suggested wording (user adapts to their own voice):

> Thanks so much for the kind words and the detailed report — it's a huge motivator!
>
> I've filed both bugs (seek regression, Home Videos library) as issues on the repo and will look at them. The Home Videos one is a one-line fix in the API parser; the seek one will need some hardware time to track down.
>
> The good news on your suggestions: the in-flight build already has a settings screen that you can open with SELECT from the libraries view — once that lands you'll be able to change the server and user without editing the config file. I'll also make the background fade to black when playback starts.
>
> The cover-art scroll and L/R "Next Up" / "Favorites" panes are great ideas. They're each meaningful enough that I want to do them as their own focused milestones rather than tacking them onto the in-flight work. I'll keep them on the roadmap.
>
> Thanks again for using it and for taking the time to write this up.

## What to file on GitHub (priority order)

1. Issue 8 (Home Videos, easy fix) — file first, ship first
2. Issue 7 (seek regression) — file, schedule hardware repro time
3. (No new issue for "settings button" — already in WIP, will be visible in the changelog)
4. (No new issue for fade-to-black — easy extension, will land in the WIP branch)
5. (No new issue for cover art or L/R panes — track in this doc for now, file as issues when work begins)

## Future feedback round doc convention

When the next batch of user feedback arrives, copy this file to `docs/feedback/round-N-reddit.md` and follow the same triage table. That gives the project a visible feedback history and prevents the same suggestion from being re-litigated across sessions.
