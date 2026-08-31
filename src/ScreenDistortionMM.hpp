#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

struct IDXGISwapChain;

namespace screen_distortion {

void initialize();
void reset(std::int64_t song_time = 0);
void update_time(bool playing, std::int64_t song_time);
void begin_noise(const std::array<std::int32_t, 7>& values,
    std::int64_t scheduled_time);
void set_effect_textures(const std::vector<std::filesystem::path>& paths);
void begin_pjsk_distortion(const std::array<std::int32_t, 12>& values,
    std::int64_t scheduled_time);
void begin_pjsk_chromatic(const std::array<std::int32_t, 20>& values,
    std::int64_t scheduled_time);
void begin_pjsk_overlay(const std::array<std::int32_t, 9>& values,
    std::int64_t scheduled_time);
// Called by DivaModLoader immediately before Present.  This only prepares the
// D3D11 hook; the actual pass is injected before the first 2D draw.
void on_frame(IDXGISwapChain* swap_chain);
void on_resize();

}
