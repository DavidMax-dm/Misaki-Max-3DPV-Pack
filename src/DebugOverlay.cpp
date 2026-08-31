#include "DebugOverlay.hpp"

#include "DebugLog.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace debug_overlay {
namespace {

using Microsoft::WRL::ComPtr;

std::mutex g_mutex;
std::vector<std::wstring> g_lines;
ComPtr<ID2D1Factory> g_d2d_factory;
ComPtr<IDWriteFactory> g_dwrite_factory;
ComPtr<IDWriteTextFormat> g_text_format;
ComPtr<ID2D1RenderTarget> g_render_target;
ComPtr<ID2D1SolidColorBrush> g_text_brush;
ComPtr<ID2D1SolidColorBrush> g_shadow_brush;
bool g_window_started = false;

std::vector<std::wstring> copy_lines() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_lines;
}

std::wstring hresult_text(const wchar_t* label, HRESULT hr) {
    std::wstringstream stream;
    stream << label << L" failed hr=0x" << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
}

void log_once(bool& flag, const std::wstring& value) {
    if (flag)
        return;

    flag = true;
    debug_log::line(value);
}

HWND find_main_window() {
    struct EnumState {
        DWORD process_id = GetCurrentProcessId();
        HWND window = nullptr;
    } state;

    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        EnumState* state = reinterpret_cast<EnumState*>(param);

        DWORD process_id = 0;
        GetWindowThreadProcessId(window, &process_id);
        if (process_id != state->process_id)
            return TRUE;

        if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER))
            return TRUE;

        RECT rect{};
        GetWindowRect(window, &rect);
        if (rect.right - rect.left < 320 || rect.bottom - rect.top < 240)
            return TRUE;

        state->window = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&state));

    return state.window;
}

void position_overlay_window(HWND overlay_window) {
    HWND game_window = find_main_window();
    if (!game_window)
        return;

    RECT rect{};
    if (!GetWindowRect(game_window, &rect))
        return;

    SetWindowPos(overlay_window, HWND_TOPMOST,
        rect.left, rect.top, 1200, 340,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        SetTimer(window, 1, 250, nullptr);
        return 0;

    case WM_TIMER:
        position_overlay_window(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);

        RECT rect{};
        GetClientRect(window, &rect);
        HBRUSH key_brush = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &rect, key_brush);
        DeleteObject(key_brush);

        SetBkMode(dc, TRANSPARENT);
        HFONT font = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        HGDIOBJ old_font = SelectObject(dc, font);

        std::vector<std::wstring> lines = copy_lines();
        int y = 8;
        for (const std::wstring& line : lines) {
            SetTextColor(dc, RGB(0, 0, 0));
            TextOutW(dc, 9, y + 1, line.c_str(), static_cast<int>(line.size()));
            SetTextColor(dc, RGB(80, 255, 120));
            TextOutW(dc, 8, y, line.c_str(), static_cast<int>(line.size()));
            y += 18;
        }

        SelectObject(dc, old_font);
        DeleteObject(font);
        EndPaint(window, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(window, 1);
        return 0;

    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void window_thread_proc() {
    const wchar_t* class_name = L"MisakiMaxSongPackDebugOverlay";

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    RegisterClassExW(&window_class);

    HWND window = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        class_name,
        L"Misaki&MaxSongPack Debug Overlay",
        WS_POPUP,
        0, 0, 1200, 340,
        nullptr, nullptr, window_class.hInstance, nullptr);

    if (!window) {
        debug_log::line(L"Fallback overlay CreateWindowEx failed");
        return;
    }

    SetLayeredWindowAttributes(window, RGB(255, 0, 255), 0, LWA_COLORKEY);
    position_overlay_window(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    debug_log::line(L"Fallback Win32 overlay created");

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool ensure_factories() {
    static bool logged_d2d_factory = false;
    static bool logged_dwrite_factory = false;
    static bool logged_text_format = false;

    if (!g_d2d_factory) {
        const HRESULT hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d_factory.GetAddressOf());
        if (FAILED(hr)) {
            log_once(logged_d2d_factory, hresult_text(L"D2D1CreateFactory", hr));
            return false;
        }
    }

    if (!g_dwrite_factory) {
        const HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(g_dwrite_factory.GetAddressOf()));
        if (FAILED(hr)) {
            log_once(logged_dwrite_factory, hresult_text(L"DWriteCreateFactory", hr));
            return false;
        }
    }

    if (!g_text_format) {
        const HRESULT hr = g_dwrite_factory->CreateTextFormat(L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            14.0f, L"", g_text_format.GetAddressOf());
        if (FAILED(hr)) {
            log_once(logged_text_format, hresult_text(L"CreateTextFormat", hr));
            return false;
        }
    }

    if (!g_text_format)
        return false;

    if (g_text_format) {
        g_text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        g_text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    return true;
}

bool ensure_render_target(IDXGISwapChain* swap_chain) {
    if (!swap_chain)
        return false;

    if (g_render_target)
        return true;

    if (!ensure_factories())
        return false;

    ComPtr<IDXGISurface> surface;
    static bool logged_get_buffer = false;
    HRESULT hr = swap_chain->GetBuffer(
        0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.GetAddressOf()));
    if (FAILED(hr)) {
        log_once(logged_get_buffer, hresult_text(L"IDXGISwapChain::GetBuffer(IDXGISurface)", hr));
        return false;
    }

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));

    static bool logged_render_target = false;
    hr = g_d2d_factory->CreateDxgiSurfaceRenderTarget(
        surface.Get(), &props, g_render_target.GetAddressOf());
    if (FAILED(hr)) {
        log_once(logged_render_target, hresult_text(L"CreateDxgiSurfaceRenderTarget", hr));
        return false;
    }

    static bool logged_shadow_brush = false;
    hr = g_render_target->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.82f), g_shadow_brush.GetAddressOf());
    if (FAILED(hr)) {
        log_once(logged_shadow_brush, hresult_text(L"CreateSolidColorBrush shadow", hr));
        return false;
    }

    static bool logged_text_brush = false;
    hr = g_render_target->CreateSolidColorBrush(
        D2D1::ColorF(0.25f, 1.0f, 0.45f, 1.0f), g_text_brush.GetAddressOf());
    if (FAILED(hr)) {
        log_once(logged_text_brush, hresult_text(L"CreateSolidColorBrush text", hr));
        return false;
    }

    debug_log::line(L"Debug overlay render target created");
    return true;
}

} // namespace

void set_lines(std::vector<std::wstring> lines) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lines = std::move(lines);
}

void start_window_overlay() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_window_started)
        return;

    g_window_started = true;
    std::thread(window_thread_proc).detach();
}

void render(IDXGISwapChain* swap_chain) {
    static bool logged_first_frame = false;
    log_once(logged_first_frame, L"OnFrame/render called");

    std::vector<std::wstring> lines = copy_lines();

    if (lines.empty() || !ensure_render_target(swap_chain))
        return;

    g_render_target->BeginDraw();

    float y = 8.0f;
    for (const std::wstring& line : lines) {
        const D2D1_RECT_F shadow_rect = D2D1::RectF(9.0f, y + 1.0f, 1200.0f, y + 22.0f);
        const D2D1_RECT_F text_rect = D2D1::RectF(8.0f, y, 1200.0f, y + 22.0f);

        g_render_target->DrawText(line.c_str(), static_cast<UINT32>(line.size()),
            g_text_format.Get(), shadow_rect, g_shadow_brush.Get());
        g_render_target->DrawText(line.c_str(), static_cast<UINT32>(line.size()),
            g_text_format.Get(), text_rect, g_text_brush.Get());

        y += 16.0f;
    }

    const HRESULT hr = g_render_target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        reset();
}

void reset() {
    g_text_brush.Reset();
    g_shadow_brush.Reset();
    g_render_target.Reset();
}

} // namespace debug_overlay
