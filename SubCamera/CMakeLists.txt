add_library(SubCamera SHARED
    src/SubCameraPlugin.cpp
)

target_compile_features(SubCamera PRIVATE cxx_std_20)
set_property(TARGET SubCamera PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(SubCamera PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(SubCamera PRIVATE
    "${DETOURS_ROOT}/include")
target_link_libraries(SubCamera PRIVATE
    "${DETOURS_ROOT}/lib/detours.lib"
    d3d11 dxgi)
set_target_properties(SubCamera PROPERTIES
    OUTPUT_NAME "SubCamera")
