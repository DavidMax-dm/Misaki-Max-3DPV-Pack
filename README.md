# Misaki-Max-3DPV-Pack

# Features

* Added additional Script tracks to bypass the original Script size limitations.
* Fixed Weight Fog and Height Fog, and made them only active in specified PVs.
* Added new Script commands for calling specific functions.
* Added specific SEKAI visual effects, which can be triggered through Script commands.

## Plugin components

The source tree builds two independent DIVA Mod Loader plugins:

* `Misaki&MaxSongPack.dll` provides the script/effect extensions, including
  the plugin-owned `SUBFRAMERENDER` command and its exported read-only state.
* `SubCamera.dll` evaluates the second `CameraRoot`, renders an independent
  offscreen scene with private SSS, and exposes it through a high-resolution
  stage RenderTexture.

`SubCamera.dll` resolves `MisakiMax_GetSubFrameRenderEnabled` dynamically at
runtime, so the projects remain independently linkable.

## Development

- Designed by DavidMax
- Code generated with OpenAI Codex based on the provided design and specifications

## References

- [ReDIVA](https://github.com/korenkonder/ReDIVA) — Referenced for research on file formats and data loading methods.

The implementation in this project was developed separately and was later expanded through reverse engineering of the game's executable.

# Special Thanks(A-Z)

* Codex
* ReDIVA Source Code by [korenkonder](https://github.com/korenkonder)
