# Issue 8: "Home Videos and Photos" library — videos don't load (Type="Video" not recognized)

> **FILED as https://github.com/bogocat/jellyfin-3ds/issues/8** (2026-06-11; Type=Video mapping shipped in PR #16, Photo handling still open. Note: the blanket IncludeItemTypes=Video idea below is wrong — it would filter Audio/folders out of every library)
> **Suggested title:** Home Videos and Photos library: videos don't load (Type=Video not recognized)
> **Suggested labels:** `area:api`, `bug`, `good first issue`
> **Citra-reproducible** — library listing works, item type just maps to `JFIN_ITEM_UNKNOWN`

---

## Lead

Reported by a v1.0.0 user on Reddit: video files in a "Home Videos and Photos" library don't load when tapped. The same MP4 files in TV/Movies libraries work fine. Root cause: `parse_item_type()` in `src/api/jellyfin.c` doesn't recognize Jellyfin's generic `Type="Video"` (used for items in Home Videos libraries, distinct from `Type="Movie"`). The parser returns `JFIN_ITEM_UNKNOWN`, so the UI's `is_video` check fails and A-to-play does nothing.

## Scope

- [ ] Add `Type="Video"` to `parse_item_type()` in `src/api/jellyfin.c:232–243` (map to `JFIN_ITEM_MOVIE`)
- [ ] Add `&IncludeItemTypes=Video` to the browse URL in `jfin_get_items()` at `src/api/jellyfin.c:417` so Photos (which the 3DS can't render) are filtered out at the server
- [ ] Verify on a Home Videos library containing both MP4s and JPGs
- [ ] Add a unit test (if/when test harness exists) or a manual test plan doc

## Out of scope

- Adding Photo support (3DS has no JPEG decoder wired; Photos would be a separate effort with image-decode work)
- Renaming `JFIN_ITEM_MOVIE` to something more generic like `JFIN_ITEM_VIDEO` (would touch many call sites; defer until v2)
- Supporting the `VideoProgram` Jellyfin type (rare; track in a follow-up issue if anyone hits it)

## Acceptance criteria

- [ ] Tapping a video in a Home Videos and Photos library starts playback
- [ ] Photo items do not appear in the browse list (filtered server-side)
- [ ] The same MP4s in Movies / TV libraries continue to work (no regression)
- [ ] `is_video` is true for `Type="Video"` items (verifiable via log_write on the browse path)

## Likely shape

Two-line fix in `src/api/jellyfin.c`:

```c
/* Inside parse_item_type(), alongside the existing entries: */
if (strcmp(type_str, "Video") == 0) return JFIN_ITEM_MOVIE;
```

And in `jfin_get_items()`, append `&IncludeItemTypes=Video` to the URL so the server excludes Photo items.

## References

- Reddit thread (incoming; URL TBD)
- `src/api/jellyfin.c:232` — `parse_item_type()` (missing `"Video"`)
- `src/api/jellyfin.c:417` — `jfin_get_items()` URL builder (no type filter)
- Jellyfin API docs: `CollectionType=homevideos` returns items with `Type=Video` or `Type=Photo`
