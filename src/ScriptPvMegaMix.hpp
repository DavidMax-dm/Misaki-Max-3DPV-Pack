#pragma once

#include <Windows.h>

#include <cstdint>

namespace script_pv_megamix {

// True only for the exact DivaMegaMix executable supported by the integrated
// FT dispatcher adapter.
bool is_supported_executable();

// Installs the MegaMix supplemental DSC hook once. Returns true when the hook is
// installed (or was already installed).
bool initialize(HMODULE plugin_module);

// Defines the native PV-frame boundary. The FT dispatcher hook only captures
// the controller belonging to this pv_game; supplemental events are emitted
// by end_pv_frame after the game's complete frame has returned.
void begin_pv_frame(void* pv_game);
void end_pv_frame(void* pv_game, float delta_time,
    std::int64_t current_time_ns);

} // namespace script_pv_megamix
