# SubCamera

`SubCamera.dll` is an independent DIVA Mod Loader plugin for Mega Mix+.
It evaluates the second `CameraRoot`, renders the scene into an independent
offscreen target with private SSS processing, and presents the result through
a high-resolution stage RenderTexture.

The plugin reads `SUBFRAMERENDER` state dynamically from the separately loaded
`Misaki&MaxSongPack.dll` export `MisakiMax_GetSubFrameRenderEnabled`. There is
no link-time dependency between the two plugin DLLs.
