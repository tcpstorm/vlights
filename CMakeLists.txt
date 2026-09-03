cmake_minimum_required(VERSION 3.20)
project(LodLightRecolor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LODLIGHT_BUILD_TESTS "Build the host-native colour-math tests" OFF)

# --- the plugin (Windows x64 only) -----------------------------------------
if(WIN32)
    enable_language(RC)
    # MinHook v1.3.4, vendored (BSD-2, see third_party/minhook/LICENSE.txt) with
    # one marked patch in src/hook.c: a failed thread snapshot no longer aborts
    # MH_EnableHook (MH_TOLERATE_FREEZE_FAILURE). See README "Caveats".
    set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook)
    add_library(minhook STATIC
        ${MINHOOK_DIR}/src/buffer.c
        ${MINHOOK_DIR}/src/hook.c
        ${MINHOOK_DIR}/src/trampoline.c
        ${MINHOOK_DIR}/src/hde/hde64.c)
    target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include)
    target_compile_definitions(minhook PRIVATE MH_TOLERATE_FREEZE_FAILURE)

    # Dear ImGui v1.92.9b, vendored (MIT, see third_party/imgui/LICENSE.txt).
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
    target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_compile_definitions(imgui PUBLIC
        IMGUI_DISABLE_DEMO_WINDOWS
        IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
        IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(imgui PUBLIC d3d11 d3dcompiler_47 dwmapi)

    add_library(LodLightRecolor SHARED
        src/Main.cpp
        src/Overlay.cpp
        src/Track.cpp
        src/NearLights.cpp
        src/Config.cpp
        src/Log.cpp
        src/Pattern.cpp
        src/LodLightRecolor.rc)
    target_include_directories(LodLightRecolor PRIVATE src)
    target_link_libraries(LodLightRecolor PRIVATE minhook imgui)
    target_compile_definitions(LodLightRecolor PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    set_target_properties(LodLightRecolor PROPERTIES PREFIX "" SUFFIX ".asi")

    if(MINGW)
        # No libgcc/libstdc++/winpthread DLL dependencies, and no accidental
        # exports.
        target_link_options(LodLightRecolor PRIVATE
            -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols -s)
        target_compile_options(LodLightRecolor PRIVATE -Wall -Wextra)
    elseif(MSVC)
        target_compile_options(LodLightRecolor PRIVATE /W4)
    endif()
endif()

# --- tests (any host) ------------------------------------------------------
if(LODLIGHT_BUILD_TESTS)
    enable_testing()
    add_executable(test_recolor tests/test_recolor.cpp)
    target_include_directories(test_recolor PRIVATE src)
    add_test(NAME recolor COMMAND test_recolor)
endif()
