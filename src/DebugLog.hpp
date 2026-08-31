#pragma once

#include <Windows.h>

#include <string>

namespace debug_log {

void init(HMODULE module, const wchar_t* plugin_name);
bool enabled();
void line(const std::wstring& value);
void line_utf8(const char* value);
void set_enabled(bool enabled);
std::wstring current_directory();

} // namespace debug_log
