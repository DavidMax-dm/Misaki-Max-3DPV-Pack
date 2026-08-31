#pragma once

#include <cstdint>

struct IDXGISwapChain;

namespace sub_camera_mm {

// Locates MegaMix+'s native camera/render-pass code after the protected image
// has been expanded in memory.  This first stage is deliberately read-only.
void initialize_probe();
void update_pv_time(std::int32_t pv_id, std::int64_t current_time_ns,
    std::int32_t field_id);
void on_frame(IDXGISwapChain* swap_chain);
void on_resize();

} // namespace sub_camera_mm
