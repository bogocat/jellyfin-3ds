# Contributing to jellyfin-3ds (humans and agents)

This repo takes contributions from people and from AI coding agents. The rules
below exist so that either can land changes that are **small, reviewable, and
safe to merge**. If you are an agent working this repo, treat this file as the
contract — follow it without being re-told.

`CLAUDE.md` describes *how the code is built and laid out*. This file describes
*how a change gets in*.

---

## The loop

```
issue  →  branch  →  small PR  →  "evaluate the code"  →  fix P0s  →  merge
```

1. **Start from an issue.** Every change traces to a GitHub issue. If one
   doesn't exist, open it first (see *Issue shape*). One issue = one unit of
   work.
2. **One concern per branch.** Branch off `main`:
   `feat/<issue>-<slug>`, `fix/<issue>-<slug>`, `docs/<slug>`,
   `ci/<slug>`, `refactor/<slug>`. Never develop on your fork's `main`.
3. **Keep the PR small.** A PR should do *one* thing. If you're adding
   subtitles *and* a manga reader *and* a downloads manager, that's three
   issues and three PRs, not one. Large multi-feature branches will be asked
   to split before review — see *Scope*.
4. **Open the PR**, fill in the template, link the issue with `Closes #N`.
5. **Request review** ("evaluate the code"). The PR is the review gate — don't
   pre-review before pushing; don't self-merge around required review.
6. **Fix P0s**, then merge (squash). Delete the branch.

## Scope — the size rule

The single most common failure mode here is one branch that bundles many
features with a long debugging history baked into its commits. Don't.

- **One concern per PR.** Roughly: if the PR description needs more than ~2
  bullet points under "What changed", it's probably more than one PR.
- A PR touching a new subsystem (a new `src/<area>/` module + its header) is
  usually the *whole* PR.
- If you discover a second, separable improvement mid-change, **file an issue
  and leave it for a follow-up PR** rather than tacking it on.
- Reviewers may decline a PR purely on scope. "Split this" is a valid P0.

## Commits

- Format: `type: brief summary` — `feat`, `fix`, `docs`, `refactor`, `chore`,
  `ci`. Imperative mood, lowercase summary.
- **No emojis. No "Generated with…" / "Co-authored-by a tool" footers.**
- Commit a coherent narrative. Squash the "fix the thing I just broke three
  commits ago" churn before review (interactive rebase or just squash on
  merge) — the merged history should read as deliberate steps, not a live
  debugging log.

## Pull request shape

Use `.github/PULL_REQUEST_TEMPLATE.md` (auto-filled). Sections:
**Summary / What changed / Out of scope / Review notes / Test plan /
Verification / Follow-ups.** Always include `Closes #<issue>`.

The **Test plan / Verification** sections are not decorative. Because CI only
*compiles* (see below), a checked, honest verification is the main signal a
reviewer has that the change actually works. Either check the boxes because you
ran it, or say explicitly what you could *not* verify and why. An all-unchecked
test plan reads as "untested".

## Issue shape

**Scope / Out of scope / Acceptance criteria / References**, plus an `area:*`
label (`area:api`, `area:audio`, `area:video`, `area:ui`, `area:net`, `ci`).
Acceptance criteria are checkboxes a reviewer can verify against.

## What CI does — and doesn't

GitHub Actions **builds every PR** (`make`, devkitARM). Green CI means *it
compiles and links* — nothing more. It does **not** run on a 3DS, so it cannot
catch crashes, races, corruption, audio glitches, or wrong rendering.

Therefore: **green CI is necessary, not sufficient.** Behavioral correctness is
established by your Verification section (Citra for UI/logic; real hardware via
`3dslink` for networking, audio, video, and SD-card I/O). If you can't run
hardware, say so and flag what's unverified so a reviewer can.

## House rules that bite on this platform

This is C on a 32-bit ARM handheld with ~128 MB RAM, a FAT32 SD card, and
cooperative threading. A few recurring hazards — violating these is a P0:

- **No unbounded input trust.** Parse server JSON and on-disk files (CBZ ZIP
  headers, cache filenames) defensively: bounds-check every length, watch for
  `uint32` overflow in `malloc(a * b)` sizes, never index `buf[n-1]` without
  proving `n > 0`.
- **Downloads to SD are write-to-`.part`, rename-on-success.** A file with its
  final name must always be complete. Sweep stale `.part` files at startup.
  (See `src/util/cache.c` for the established pattern — match it; don't write
  final names directly.)
- **Don't delete or truncate a file that may be open** for playback/reading
  under the sdmc devoptab — stop the consumer first.
- **Cross-thread flags go through `__atomic_*`**, not bare `volatile`. Follow
  the ring-buffer pattern in `src/audio/player.c`.
- **Snprintf truncation is silent.** Check the return value when building URLs
  and paths; a half-built URL fails quietly.
- **Reuse, don't fork.** Before adding a second download/cache/playback path,
  build on the existing module. Two parallel implementations of the same thing
  is the bug, even before any line is wrong.

## Coordinate

Before starting, skim open PRs and issues. If your change overlaps an open PR
(same files/feature), say so in the issue and coordinate — don't build a
competing branch that silently re-solves a solved problem.
