cmake_minimum_required(VERSION 3.20)

project(MisakiMaxSongPack LANGUAGES CXX)

set(DETOURS_ROOT "" CACHE PATH "Path to a Microsoft Detours build containing include/detours.h and lib/detours.lib")
if(NOT EXISTS "${DETOURS_ROOT}/include/detours.h" OR
   NOT EXISTS "${DETOURS_ROOT}/lib/detours.lib")
    message(FATAL_ERROR
        "Set DETOURS_ROOT to a Microsoft Detours directory containing "
        "include/detours.h and lib/detours.lib")
endif()

file(GLOB FOG_SHADER_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/fog_shaders/*.cso")
if(NOT FOG_SHADER_FILES)
    message(FATAL_ERROR "No embedded fog shader resources were found")
endif()

set(FOG_SHADER_RC "${CMAKE_CURRENT_BINARY_DIR}/FogShaderOverrides.rc")
file(WRITE "${FOG_SHADER_RC}" "#include <windows.h>\n")
foreach(SHADER IN LISTS FOG_SHADER_FILES)
    get_filename_component(RESOURCE_NAME "${SHADER}" NAME_WE)
    file(TO_CMAKE_PATH "${SHADER}" RESOURCE_PATH)
    file(APPEND "${FOG_SHADER_RC}"
        "${RESOURCE_NAME} RCDATA \"${RESOURCE_PATH}\"\n")
endforeach()

add_library(MisakiMaxSongPack SHARED
    src/DebugLog.cpp
    src/DebugOverlay.cpp
    src/EffectScript.cpp
    src/EffectScriptPlugin.cpp
    src/FogDepthHeightFixMM.cpp
    src/ScreenDistortionMM.cpp
    src/ScriptPvDscParser.cpp
    src/ScriptPvMegaMix.cpp
    src/SubCameraMMDisabled.cpp
    "${FOG_SHADER_RC}"
)

target_compile_features(MisakiMaxSongPack PRIVATE cxx_std_20)
set_property(TARGET MisakiMaxSongPack PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(MisakiMaxSongPack PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(MisakiMaxSongPack PRIVATE
    "${DETOURS_ROOT}/include")
target_link_libraries(MisakiMaxSongPack PRIVATE
    "${DETOURS_ROOT}/lib/detours.lib"
    d2d1 dwrite d3d11 d3dcompiler ole32 windowscodecs)
set_target_properties(MisakiMaxSongPack PROPERTIES
    OUTPUT_NAME "Misaki&MaxSongPack")

include(CTest)
if(BUILD_TESTING)
    add_executable(ScriptPvIntegratedParserTests
        src/ScriptPvDscParser.cpp
        tests/ScriptPvDscParserTests.cpp)
    target_compile_features(ScriptPvIntegratedParserTests PRIVATE cxx_std_20)
    target_include_directories(ScriptPvIntegratedParserTests PRIVATE src)
    add_test(NAME ScriptPvIntegratedParserTests
        COMMAND ScriptPvIntegratedParserTests)
endif()
