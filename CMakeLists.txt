cmake_minimum_required(VERSION 3.20)

# One source of truth for the version: the VERSION file at the repo root.
# CMake turns it into project(VERSION), a generated version.h, and the
# Windows version resource; CI and the release workflow read the same file.
file(STRINGS VERSION VLIGHTS_VERSION LIMIT_COUNT 1)
string(STRIP "${VLIGHTS_VERSION}" VLIGHTS_VERSION)
if(NOT VLIGHTS_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain MAJOR.MINOR.PATCH, got '${VLIGHTS_VERSION}'")
endif()
project(VLights VERSION ${VLIGHTS_VERSION} LANGUAGES C CXX)
set(VLIGHTS_VER_MAJOR ${PROJECT_VERSION_MAJOR})
set(VLIGHTS_VER_MINOR ${PROJECT_VERSION_MINOR})
set(VLIGHTS_VER_PATCH ${PROJECT_VERSION_PATCH})
set(VLIGHTS_AUTHOR "Storm" CACHE STRING "Author name written into the .asi version info")
set(VLIGHTS_URL "https://github.com/tcpstorm/vlights" CACHE STRING "Project URL written into the .asi version info")
string(TIMESTAMP VLIGHTS_YEAR "%Y")
configure_file(res/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/vlights/version.h @ONLY)
message(STATUS "VLights ${VLIGHTS_VERSION} by ${VLIGHTS_AUTHOR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VLIGHTS_BUILD_TESTS "Build the host-native colour-math tests" OFF)

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

    # --- identity baked into the .asi (Properties > Details) ---------------
    configure_file(res/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)

add_library(VLights SHARED
        src/main.cpp
        src/plugin/plugin.cpp
        src/plugin/config.cpp
        src/plugin/log.cpp
        src/hook/pattern.cpp
        src/game/lod_lights.cpp
        src/game/near_lights.cpp
        src/game/textures.cpp
        src/game/track.cpp
        src/ui/menu.cpp
        res/VLights.rc
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
    target_include_directories(VLights PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(VLights PRIVATE minhook imgui)
    target_compile_definitions(VLights PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    set_target_properties(VLights PROPERTIES PREFIX "" SUFFIX ".asi")

    if(MINGW)
        # No libgcc/libstdc++/winpthread DLL dependencies, and no accidental
        # exports.
        target_link_options(VLights PRIVATE
            -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols -s)
        target_compile_options(VLights PRIVATE -Wall -Wextra)
    elseif(MSVC)
        target_compile_options(VLights PRIVATE /W4)
    endif()
endif()

# --- tests (any host) ------------------------------------------------------
if(VLIGHTS_BUILD_TESTS)
    enable_testing()
    add_executable(test_recolor tests/test_recolor.cpp)
    target_include_directories(test_recolor PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    add_test(NAME recolor COMMAND test_recolor)
endif()
