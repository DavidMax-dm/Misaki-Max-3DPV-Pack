# Release source manifest

This directory is the public release source worktree. It contains two
independently linked DIVA Mod Loader plugins in one CMake/Visual Studio build.

## Included components

| Area | Source files |
| --- | --- |
| Plugin entry point and lifecycle | `src/EffectScriptPlugin.cpp` |
| `script_effect` and ScreenFX | `src/EffectScript.cpp`, `src/ScreenDistortionMM.cpp` |
| Integrated `script_pv` | `src/ScriptPvMegaMix.cpp`, `src/ScriptPvDscParser.cpp` |
| Height-fog corrections | `src/FogDepthHeightFixMM.cpp`, `resources/fog_shaders/*.cso` |
| Release logging implementation | `src/DebugLog.cpp` |
| Disabled sub-camera ABI stub | `src/SubCameraMMDisabled.cpp` |
| Independent secondary-camera renderer | `SubCamera/src/SubCameraPlugin.cpp` |
| Parser verification | `tests/ScriptPvDscParserTests.cpp`, `tests/EffectScriptExtensionTests.cpp` |

Experimental `SubCameraMM.cpp`, extended-movie sources, generated shader dumps,
RenderDoc captures, logs, game assets, and build products are intentionally
excluded.

`Misaki&MaxSongPack.dll` owns the plugin-side `script_effect` parser and exports
`MisakiMax_GetSubFrameRenderEnabled`. `SubCamera.dll` resolves that export at
runtime; there is no link-time dependency between the DLLs.

## Installed DLL reference

The DLL installed in the release mod folder when this snapshot was prepared is
preserved under `reference_binary/` for binary comparison:

- File: `Misaki&MaxSongPack.dll`
- Size: `1,129,984` bytes
- SHA-256: `C206BAB1F2F71ED2571B23A83863E290D6E5DB99D4017EA57EF52B0FEC4EDADC`
- PE timestamp: `2026-09-02 16:34:05 +08:00`

That binary does not byte-match any retained build output. Therefore the Git
commit above is the last clean, complete release-line source baseline, while
the DLL is retained as the authoritative behavior/reference artifact. Do not
claim a reproducible byte-identical build until the historical intermediate
source state or matching object files are recovered.

## Build

Configure `DETOURS_ROOT` to a Detours source/build tree, then use the x64 Release
configuration:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DDETOURS_ROOT="<path-to-detours>"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The generated `MisakiMaxSongPack.sln` contains independent
`MisakiMaxSongPack` and `SubCamera` projects, and building the solution produces
`Misaki&MaxSongPack.dll` and `SubCamera.dll`.
