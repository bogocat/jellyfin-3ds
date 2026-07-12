# Contributing to jellyfin-3ds with AI agents

> **The one-line version:** Your code was good. The mess in #24 was a *process*
> problem, and using an agent the right way makes the process **cleaner**, not
> messier — because the agent works inside a discipline you give it. This doc is
> that discipline, using our own repo as the example.

`AGENTS.md` is the **contract** every coding agent reads automatically (Claude
Code, Cursor, Copilot, others). It already encodes the rules below. This doc is
the *how to actually do it on a Tuesday* companion — read it once, then let
`AGENTS.md` be your reference.

---

## Part 1 — The loop (memorize this shape)

```
   issue            one concern, filed first        ┐
     │                                              │
     ▼                                              │
   branch    feat/<issue#>-<slug> off origin/main   │  one unit
     │         NEVER develop on your fork's main    │  of work
     ▼                                              │
   agent     "work issue #N, follow AGENTS.md"      │
     │         small, scoped, one thing             │
     ▼                                              │
   PR        Closes #N, filled template, verified   │
     │                                              │
     ▼                                              │
   review    "evaluate the code" → fix P0s → merge  ┘
```

**The whole point:** the loop keeps every change small and traceable. An agent
that's told *"work issue #N"* and pointed at `AGENTS.md` will produce one small,
reviewable PR. An agent (or a human) given *"add subtitles and a manga reader
and a downloads manager"* will produce #24.

---

## Part 2 — What went wrong with #24, and the rule that prevents each

| What happened in #24 | The rule that stops it | Where it lives |
|---|---|---|
| **6 features in one PR** — unreviewable, unmergeable | One concern per branch/PR. If "What changed" needs >2 bullets, it's >1 PR. | AGENTS.md → *Scope* |
| **Re-implemented the cache/offline layer** that #23 had just added | Reuse, don't fork. Build on `src/util/cache.c`; a second implementation is the bug. | AGENTS.md → *House rules* |
| **Committed `debug.log` / `debug_1.log`** (~2700 lines) | Add to `.gitignore` **and** `git rm --cached` the already-tracked file. Stage specific paths, never `git add -A`. | this doc, Part 5 |
| **Fork's `main` diverged 70 ahead / 14 behind** → rebased copies of upstream commits, history hell | Branch off **upstream `main`** per feature. Your fork's `main` is a mirror, not a workspace. | this doc, Part 4 |
| **Debugging churn baked into history** ("fix the thing I broke 3 commits ago") | Squash before review. Merged history reads as deliberate steps, not a debug log. | AGENTS.md → *Commits* |

None of these are about skill. Every one is a habit the loop makes automatic.

---

## Part 3 — Worked example: land the manga reader the clean way

Let's get *your* manga/CBZ reader in — but as a small, safe PR. This is exactly
the loop, on your own code. Two real bugs were found in review; fixing them is a
perfect first agent task because it's small, concrete, and verifiable.

### Step 1 — File the issue first
```
Title: feat: CBZ/manga reader (normal + split-screen)
Labels: area:ui, enhancement
Body:   Scope / Out of scope / Acceptance criteria / References
        (the AGENTS.md "Issue shape")
```
You now have issue **#N**. Every later step points at it.

### Step 2 — Branch off upstream main
```bash
git remote add upstream https://github.com/bogocat/jellyfin-3ds.git   # once
git fetch upstream
git switch -c feat/<N>-manga-reader upstream/main     # NEVER your fork's main
```

### Step 3 — Cherry-pick your reader code, then hand the bugs to the agent
Bring over `src/ui/reader.c`, `src/util/cbz.c`, and their headers. **Before**
you ask for review, fix the two P0 bugs review will flag anyway:

**Bug A — out-of-bounds read on empty filenames** (`cbz.c`):
```c
int nread = (fname_len < 127) ? fname_len : 127;
if (fread(name, 1, (size_t)nread, c->fp) < (size_t)nread) break;
if (nread == 0) continue;                 /* ← ADD: name[nread-1] is OOB when 0 */
if (name[nread - 1] == '/' || name[nread - 1] == '\\') continue;
```
`name[nread-1]` reads off the stack when `fname_len == 0`. This is literally the
AGENTS.md rule: *"never index `buf[n-1]` without proving `n > 0`."*

**Bug B — `uint32` overflow → heap overflow** (`cbz.c`):
```c
/* decomp buffer size must not overflow on a crafted/odd CBZ */
size_t decomp_sz = e->decomp_sz > 0 ? (size_t)e->decomp_sz
                                    : (size_t)e->comp_sz * 6;
if (decomp_sz == 0 || decomp_sz > 64u * 1024 * 1024) { free(comp); return NULL; }
u8 *out = malloc(decomp_sz);
```
`e->comp_sz * 6` in `uint32_t` can wrap to a tiny allocation; `inflate` then
blows past it. This is the AGENTS.md rule: *"watch for `uint32` overflow in
`malloc(a * b)` sizes."*

### Step 4 — Tell the agent (this is the whole prompt)
> Work issue #N. Follow `AGENTS.md`. Fix the two P0 bugs in `src/util/cbz.c`
> (empty-filename OOB read; `uint32` overflow in the decompress buffer). Keep
> the change to those two fixes. Build with `make`, verify the reader opens a
> CBZ in Citra. Open a PR that `Closes #N`.

That's it. Scoped task, pointed at the contract, one concern.

### Step 5 — Verify honestly, then PR
- `make` compiles (CI will check this, but run it).
- Open a CBZ in Citra; confirm page order + an empty-filename entry doesn't
  crash. Check the Test plan / Verification boxes **because you ran them** —
  green CI only proves it *links*, not that it works on a 3DS (see AGENTS.md
  → *What CI does — and doesn't*).

---

## Part 4 — Directing an agent well (the part most people skip)

The agent is the **executor**; you are the **director**. Bad direction = bad
result, same as a human.

**Good prompts are small and bounded:**
- ✅ "Work issue #N. Follow AGENTS.md. Touch only `src/video/`. Build + verify, then PR."
- ✅ "In `cbz.c`, the `name[nread-1]` access is an OOB read when `fname_len==0`. Add a guard. One commit."
- ❌ "Add subtitles, a manga reader, and fix the downloads."  *(→ that's #24)*
- ❌ "Make the app better."

**Habits that prevent messes:**
1. **One issue per branch.** Finish and merge before starting the next.
2. **Branch off `upstream/main`, fetched fresh.** Your fork's `main` drifts.
3. **Commit specific paths,** e.g. `git add src/util/cbz.c include/util/cbz.h`.
   Never `git add -A` — that's how `debug.log` sneaks in.
4. **Squash churn before review.** `git rebase -i upstream/main` to collapse
   "wip / fix / fix the fix" into one clean commit.
5. **Tell the agent to read AGENTS.md.** It's already written; don't re-explain
   the rules in every prompt.

---

## Part 5 — The mess-makers (anti-patterns)

- **Developing on `main`.** Always a feature branch. `main` is a mirror.
- **`git add -A` / `git add .`** Stage explicit files. Add `debug*.log`,
  `*.dmp`, `test-files/` to `.gitignore` *and* `git rm --cached` anything
  already tracked.
- **Bundling.** Discovered a second improvement mid-PR? File an issue, leave it
  for a follow-up PR.
- **Forking a solved module.** Before writing a new download/cache/playback
  path, check `src/util/cache.c` — build on it.
- **Skipping verification.** Unchecked Test-plan boxes read as "untested."

---

## Cheatsheet

```bash
# start
git fetch upstream
git switch -c feat/<N>-<slug> upstream/main

# stage carefully (never -A)
git add src/<path> include/<path>

# clean up history before review
git rebase -i upstream/main

# push + PR
git push -u origin feat/<N>-<slug>
gh pr create --base main --fill        # fills the PR template, add "Closes #N"
```

The loop is the whole game. Run it once per concern and your PRs land in an
hour instead of stalling for a rework.
