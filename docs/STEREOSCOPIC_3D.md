# Stereoscopic 3D Video — Implementation Plan

> Date: 2026-03-30
> Status: **Implemented** (HSBS + FSBS; pending hardware verification — see test plan below)
> Test content: "Peace World 3D SBS Test" (1280x720 HalfSideBySide, 60s) in Jellyfin library

## Overview

Play Side-by-Side (SBS) 3D movies from Jellyfin using the 3DS's autostereoscopic display. The 3DS top screen has a parallax barrier that shows different images to each eye — no glasses needed.

## How SBS 3D Works

```
Source file (1280x720 HalfSideBySide):
┌──────────────┬──────────────┐
│  LEFT EYE    │  RIGHT EYE   │
│  (640x720)   │  (640x720)   │
└──────────────┴──────────────┘

Jellyfin transcodes to 800x450:
┌──────────┬──────────┐
│ LEFT EYE │RIGHT EYE │
│ (400x450)│(400x450) │
└──────────┴──────────┘

3DS splits and renders:
  Left eye target (400x240)    Right eye target (400x240)
┌────────────────────┐       ┌────────────────────┐
│    ░░ letterbox ░░ │       │    ░░ letterbox ░░ │
│ ┌────────────────┐ │       │ ┌────────────────┐ │
│ │   LEFT (400x   │ │       │ │  RIGHT (400x   │ │
│ │     225)       │ │       │ │     225)       │ │
│ └────────────────┘ │       │ └────────────────┘ │
│    ░░ letterbox ░░ │       │    ░░ letterbox ░░ │
└────────────────────┘       └────────────────────┘

Parallax barrier interleaves → viewer sees 3D
```

## Data Flow

```
Current (2D):
  Jellyfin → 400x224 TS → MVD decode → 1 texture → 1 render target

3D SBS:
  Jellyfin → 800xN TS → MVD decode → split → 2 textures → 2 render targets
                                        ↓
                              left half → tex_left → GFX_LEFT
                              right half → tex_right → GFX_RIGHT
```

## Changes Required

### 1. API: Detect 3D content and request wider transcode

**File: `include/api/jellyfin.h`**
- Add `JFIN_ITEM_3D_NONE`, `JFIN_ITEM_3D_HSBS`, `JFIN_ITEM_3D_HTAB` enum
- Add `int video_3d_format` field to `jfin_item_t`

**File: `src/api/jellyfin.c`**
- Parse `Video3DFormat` in `parse_item()` — look for "HalfSideBySide", "FullSideBySide", "HalfTopAndBottom"
- In `jfin_get_video_stream()`: if item is SBS, use `MaxWidth=800` (no MaxHeight constraint) instead of `MaxWidth=400&MaxHeight=240`

### 2. Video player: SBS-aware decode and frame splitting

**File: `include/video/video_player.h`**
- Add `bool is_3d` and `int sbs_mode` to `video_status_t`
- Add `bool video_player_play_3d(...)` or add a `bool is_3d` param to existing play function

**File: `src/video/video_player.c`**

State changes:
```c
bool            is_3d;           /* true for SBS content */
int             full_width;      /* 800 for SBS, 400 for 2D */
```

MVD decode changes:
- MVD init with wider resolution: `mvd_init(&mvd, 800, 450)` for SBS
- MVD work buffer is sized by resolution — 800x450 needs more than 400x224
- Verify MVD handles 800x450 (within its 854x480 limit from browser)

Frame queue changes:
- Each queued frame is now 800xN instead of 400xN
- `fq_init()` allocates larger frame buffers for SBS

Convert thread changes — **the key new logic**:
```c
if (s_vp.is_3d) {
    /* Split SBS frame: left half → tex[0], right half → tex[1] */
    /* Source: 800xN BGR565, left eye = columns 0-399, right = 400-799 */

    /* Tile left half into left eye texture */
    for (y = 0; y < h; y++) {
        const u8 *row = frame + y * full_width * 2;  /* full row */
        /* Tile columns 0..399 into tex_left */
        tile_half_row(tex_left_data, row, 0, 400, y);
    }

    /* Tile right half into right eye texture */
    for (y = 0; y < h; y++) {
        const u8 *row = frame + y * full_width * 2;
        /* Tile columns 400..799 into tex_right */
        tile_half_row(tex_right_data, row + 400 * 2, 0, 400, y);
    }
}
```

### 3. Rendering: Dual render targets

**File: `src/ui/ui.c`**

Init changes:
```c
/* In ui_init(), enable 3D */
/* We always create both targets — only use right when playing 3D */
s_top_left  = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
s_top_right = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
```

Render changes in `ui_render_now_playing()`:
```c
if (is_3d_playing) {
    gfxSet3D(true);

    /* Left eye */
    C2D_TargetClear(s_top_left, 0x000000FF);
    C2D_SceneBegin(s_top_left);
    video_player_render_frame_left();

    /* Right eye (only if 3D slider > 0) */
    float slider = osGet3DSliderState();
    if (slider > 0.0f) {
        C2D_TargetClear(s_top_right, 0x000000FF);
        C2D_SceneBegin(s_top_right);
        video_player_render_frame_right();
    }
} else {
    gfxSet3D(false);
    /* Normal 2D rendering (current code) */
}
```

### 4. Video player render: Two frame functions

**File: `src/video/video_player.c`**

New functions:
```c
void video_player_render_frame_left(void);   /* draws left eye texture */
void video_player_render_frame_right(void);  /* draws right eye texture */
```

Or modify existing `video_player_render_frame()` to accept an eye parameter.

Double-buffered textures become quadruple-buffered:
```c
C3D_Tex  frame_tex[4];  /* [0,1] = left eye double buf, [2,3] = right eye double buf */
/* Or simpler: */
C3D_Tex  frame_tex_left[2];
C3D_Tex  frame_tex_right[2];
```

### 5. UI: 3D indicator

**File: `src/ui/ui.c`**

- Show "3D" badge on now-playing screen when playing SBS content
- Show "3D" next to movie name in browse list if `Video3DFormat` is set

### 6. 3D slider integration

```c
float slider = osGet3DSliderState();
if (slider == 0.0f) {
    /* 3D disabled — render only left eye (saves GPU) */
    /* Or render 2D: both eyes get left image */
}
```

## Memory Budget (SBS vs 2D)

| Component | 2D (current) | SBS 3D |
|---|---|---|
| MVD work buffer | ~10 MB | ~12 MB (800x450) |
| MVD output frame | 400×224×2 = 175 KB | 800×450×2 = 703 KB |
| Frame queue (6 slots) | 6 × 175 KB = 1 MB | 6 × 703 KB = 4.1 MB |
| Convert textures (2) | 2 × 512×256×2 = 512 KB | 4 × 512×256×2 = 1 MB |
| **Total delta** | — | **+4 MB** |

Total linear memory for 3D: ~17 MB (vs ~13 MB for 2D). Still within the ~30 MB budget.

## Performance Considerations

- MVD decodes 800x450 instead of 400x224 — ~4× more pixels, but still within hardware limits
- Convert thread tiles two halves instead of one — ~2× tiling time
- GPU renders two textured quads instead of one — minimal impact (simple draw call)
- Net impact: decode FPS may drop from 24 to ~18-20. Should still be watchable.
- When 3D slider is at 0, skip right eye entirely — same performance as 2D

## Implementation Order

1. **API**: Parse `Video3DFormat`, add to item type (~20 min)
2. **Transcode URL**: `MaxWidth=800` for SBS items (~5 min)
3. **Render targets**: Add right eye target, `gfxSet3D()` toggle (~15 min)
4. **Convert thread**: Split frame into left/right halves during tiling (~30 min)
5. **Video player**: Separate left/right render functions (~15 min)
6. **UI**: Pass 3D flag through, render both eyes (~15 min)
7. **Test on hardware** with Peace World SBS clip
8. **3D slider**: Respect slider state (~5 min)

**Estimated total: ~2 hours of implementation**

## Test Plan

1. Browse to "Peace World 3D SBS Test" in Movies
2. Press A to play
3. Verify 3D slider on 3DS shows depth effect
4. Verify left/right eye content differs (close one eye)
5. Verify audio still syncs
6. Verify seeking works
7. Turn 3D slider to 0 — should show 2D (left eye only)
8. Play a normal 2D movie — should render as before (no regression)

## Future: Top-and-Bottom 3D

Same approach but split vertically instead of horizontally:
- Request `MaxWidth=400&MaxHeight=480`
- Top half → left eye, bottom half → right eye
- Lower priority since most 3D content is SBS
