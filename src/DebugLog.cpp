#include "DebugLog.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace debug_log {
namespace {

std::mutex g_mutex;
std::filesystem::path g_log_path;
std::wstring g_plugin_name = L"Misaki&MaxSongPack";
bool g_enabled = false;
bool g_initialized = false;

std::string narrow_utf8(const std::wstring& value) {
    if (value.empty())
        return {};

    const int length = WideCharToMultiByte(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};

    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

void append_unlocked(const std::wstring& value) {
    if (g_log_path.empty())
        return;

    std::ofstream stream(g_log_path, std::ios::app | std::ios::binary);
    if (!stream)
        return;

    stream << narrow_utf8(value) << "\r\n";
}

} // namespace

void init(HMODULE module, const wchar_t* plugin_name) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (plugin_name && *plugin_name)
        g_plugin_name = plugin_name;

    if (!g_enabled)
        return;

    wchar_t path[MAX_PATH]{};
    if (module && GetModuleFileNameW(module, path, _countof(path))) {
        g_log_path = std::filesystem::path(path).parent_path() / (g_plugin_name + L".debug.log");
    }
    else {
        g_log_path = std::filesystem::current_path() / (g_plugin_name + L".debug.log");
    }

    if (!g_initialized) {
        std::ofstream stream(g_log_path, std::ios::trunc | std::ios::binary);
        if (stream)
            stream << narrow_utf8(g_plugin_name) << " debug log\r\n";

        g_initialized = true;
    }
}

bool enabled() {
    return g_enabled;
}

void line(const std::wstring& value) {
    if (!g_enabled)
        return;

    OutputDebugStringW((L"[" + g_plugin_name + L"] ").c_str());
    OutputDebugStringW(value.c_str());
    OutputDebugStringW(L"\n");

    std::lock_guard<std::mutex> lock(g_mutex);
    append_unlocked(std::to_wstring(GetTickCount64()) + L" " + value);
}

void line_utf8(const char* value) {
    if (!value)
        return;

    const int length = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (length <= 0)
        return;

    std::wstring wide(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, wide.data(), length);
    line(wide);
}

void set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_enabled = enabled;
    if (!g_enabled) {
        g_initialized = false;
        g_log_path.clear();
    }
}

std::wstring current_directory() {
    wchar_t path[MAX_PATH]{};
    GetCurrentDirectoryW(_countof(path), path);
    return path;
}

} // namespace debug_log
