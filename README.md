# vlc-st2110-player
VLC plugin to receive and monitor uncompressed SMPTE ST 2110 (paste an SDP, get video + audio)

## Install

Copy [src/st2110_sdp_loader.lua](src/st2110_sdp_loader.lua) into VLC's Lua extensions directory, then restart VLC and enable it via View / Tools → Plugins and extensions.

| OS | Path |
|---|---|
| Windows | `%APPDATA%\vlc\lua\extensions\` |
| macOS | `~/Library/Application Support/org.videolan.vlc/lua/extensions/` |
| Linux | `~/.local/share/vlc/lua/extensions/` |

See [docs/ST2110_SDP_Loader_仕様書.md](docs/ST2110_SDP_Loader_仕様書.md) for the full specification.

## Video receive module (10bit ST 2110-20)

[receive-module/st2110.c](receive-module/st2110.c) is a VLC `access_demux` C plugin that receives and depacketizes ST 2110-20 (RFC 4175) 10bit 4:2:2 GPM video and outputs `I422_10L`. It is what actually lets 10bit video show up; the Lua extension alone cannot decode it. See [docs/vlc-st2110_receive-module_仕様書.md](docs/vlc-st2110_receive-module_仕様書.md) for the full specification.

Build (Linux/macOS, requires the VLC 3.0.x plugin SDK / `pkg-config vlc-plugin`):

```
cd receive-module
make
sudo make install   # or set VLC_PLUGIN_PATH to point at the build dir instead
```

Then set `USE_CUSTOM_VIDEO = true` at the top of `src/st2110_sdp_loader.lua` so the Lua extension routes video through `st2110://` instead of the stock RTP path.

This has not been build- or hardware-tested yet (see the spec's Open Questions §12 — VLC version, line-numbering origin, and loss-threshold tuning still need verification against a real ST 2110-20 sender).
