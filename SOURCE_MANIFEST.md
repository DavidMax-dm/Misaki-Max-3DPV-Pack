# Source manifest

The target DLL was matched by SHA-256 to the historical build output at
`EffectScriptPlugin/build_native_trace/Release/Misaki&MaxSongPack.dll`.

Its linker record showed the following compilation units:

| Area | Source files |
| --- | --- |
| Entry point and runtime | `EffectScriptPlugin.cpp` |
| script_effect | `EffectScript.cpp`, `ScreenDistortionMM.cpp` |
| script_pv | `ScriptPvMegaMix.cpp`, `ScriptPvDscParser.cpp` |
| Fog correction | `FogDepthHeightFixMM.cpp`, `resources/fog_shaders/*.cso` |
| Release diagnostics | `DebugLog.cpp` (permanently disabled in the clean release) |
| Sub-camera ABI | `SubCameraMMDisabled.cpp` |

All corresponding project headers are present under `src/`. The parser test is
under `tests/`.

The clean release does not compile the in-game debug overlay or the experimental
sub-camera implementation. ScreenFX probe output is compiled to no-ops.

The published target did **not** contain `SubCameraMM.cpp`, `ExtendedMovieMM.cpp`
or `ExtendedMovieStandalone.cpp`; those experimental/separate targets are not
included in this snapshot.
