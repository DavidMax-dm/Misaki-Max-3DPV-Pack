#include "SubCameraMM.hpp"

#include <Windows.h>

#include <atomic>

namespace {

using UpdatePVStateFn = void(__cdecl*)(std::int32_t, std::int64_t,
    std::int32_t);

std::atomic<UpdatePVStateFn> g_update_pv_state{};
std::atomic<bool> g_lookup_attempted{};

UpdatePVStateFn find_subcamera_bridge() {
    UpdatePVStateFn callback = g_update_pv_state.load(std::memory_order_acquire);
    if (callback)
        return callback;
    // All configured DLLs are loaded before Init.  Do a single lookup: this
    // compatibility object must remain completely inert when MMSubCamera is
    // absent, exactly as it was before the bridge existed.
    if (g_lookup_attempted.exchange(true, std::memory_order_acq_rel))
        return nullptr;
    const HMODULE module = GetModuleHandleW(L"MMSubCamera.dll");
    callback = module ? reinterpret_cast<UpdatePVStateFn>(
        GetProcAddress(module, "MMSubCamera_UpdatePVState")) : nullptr;
    g_update_pv_state.store(callback, std::memory_order_release);
    return callback;
}

} // namespace

namespace sub_camera_mm {

void initialize_probe() {}

void update_pv_time(std::int32_t pv_id, std::int64_t time_ns,
    std::int32_t field_id) {
    if (const UpdatePVStateFn callback = find_subcamera_bridge())
        callback(pv_id, time_ns, field_id);
}

// Compatibility with the preserved stable EffectScriptPlugin object, which
// predates the field-id parameter.
void update_pv_time(std::int32_t pv_id, std::int64_t time_ns) {
    if (const UpdatePVStateFn callback = find_subcamera_bridge())
        callback(pv_id, time_ns, -1);
}

void on_frame(IDXGISwapChain*) {}

void on_resize() {}

} // namespace sub_camera_mm
