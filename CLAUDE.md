# jellyfin-3ds

Native Jellyfin media client for Nintendo 3DS homebrew.

Repo lives at github.com/bogocat/jellyfin-3ds (the old serrattleballs
pseudonym is retired). CI builds every PR via GitHub Actions.

## Build

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with 3DS toolchain:

```bash
# Install dependencies
sudo dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib 3ds-libmpg123 3ds-libopus 3ds-opusfile 3ds-libvorbisidec 3ds-libogg

# Download cJSON (single-file JSON parser)
curl -L https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h -o include/api/cJSON.h
curl -L https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c -o src/api/cJSON.c

# Build
make

# Deploy to 3DS over WiFi (requires 3dslink on device)
3dslink jellyfin-3ds.3dsx
```

## Architecture

```
src/
  main.c              Main loop: init services, input, update, render
  api/
    jellyfin.c         Jellyfin REST API client (libcurl + cJSON)
    cJSON.c            JSON parser (vendored, single-file)
  audio/
    player.c           Streaming audio player (libcurl -> mpg123 -> NDSP)
  video/
    (phase 2)          H.264 decode via MVD (New 3DS) or FFmpeg (Old 3DS)
  ui/
    ui.c               Dual-screen citro2d UI (touch + dpad)
  util/
    config.c           SD card config persistence
```

## Key References

- **ThirdTube** (YouTube 3DS client): github.com/windows-server-2003/ThirdTube — closest architecture match
- **Switchfin** (Jellyfin Switch/Vita client): github.com/dragonflylee/switchfin — API layer reference
- **Video player for 3DS**: github.com/Core-2-Extreme/Video_player_for_3DS — FFmpeg cross-compile, MVD hardware decode
- **ctrmus**: github.com/deltabeard/ctrmus — NDSP audio patterns

## Testing

- UI/logic: Citra emulator (networking is broken in emulator)
- Networking/audio: Real 3DS hardware with Luma3DS CFW via 3dslink
- Need `dspfirm.cdc` at `sdmc:/3ds/dspfirm.cdc` for audio (dump via DSP1 on real hardware)

## Controls

| Button | Login | Browse | Now Playing |
|--------|-------|--------|-------------|
| D-Pad  | Field select | List navigate | — |
| A      | Edit field | Enter/Play | Pause/Resume |
| B      | — | Back | Return to browse |
| X      | — | — | Stop playback |
| Y      | — | Now Playing view | — |
| R      | Connect | — | — |
| START  | Exit | Exit | Exit |
| Touch  | — | Tap to select | — |
