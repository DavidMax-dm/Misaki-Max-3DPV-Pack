#pragma once

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>

struct ID3D11DeviceContext;

namespace fog_depth_height_fix {

// Installs the early D3D11 device-creation hook. Safe to call more than once.
void initialize(HMODULE plugin_module);

// Covers the case where the game created its device before the DLL was loaded.
void ensure_device_hooks(IDXGISwapChain* swap_chain);

// Updates the active PV used by narrowly-scoped compatibility fixes.
void update_pv_id(int32_t pv_id);

// Temporarily selects paired character-only fog shaders for one draw call.
bool begin_character_draw(ID3D11DeviceContext* context);
void end_character_draw(ID3D11DeviceContext* context);

}
