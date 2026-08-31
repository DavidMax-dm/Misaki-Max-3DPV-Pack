#pragma once

#include <dxgi.h>

#include <string>
#include <vector>

namespace debug_overlay {

void set_lines(std::vector<std::wstring> lines);
void start_window_overlay();
void render(IDXGISwapChain* swap_chain);
void reset();

} // namespace debug_overlay
