cmake_minimum_required(VERSION 3.21)
project(Retro3DO CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(RETRO3DO_BUILD_TESTS "Build the host test suite" ON)
option(RETRO3DO_USE_SYSTEM_SDL "Link the system SDL3 instead of the vendored one" OFF)

# ---------------------------------------------------------------------------
# The emulator core.
#
# Deliberately free of any platform or windowing dependency: it is linked
# unchanged into the app, into the tests, and (later) into the Android and iOS
# targets. If something in here ever needs a platform header, that is the signal
# that it belongs in src/platform instead.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# libchdr — reading CHD, which is how nearly every 3DO dump in the wild is
# stored. BSD-3-Clause, and self-contained: it brings its own LZMA, miniz and
# zstd, so there is nothing to find on the host and nothing extra to cross
# compile for Android or iOS. See docs/THIRD_PARTY.md.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZSTD OFF CACHE BOOL "" FORCE)
# Hand back cooked user data rather than sync+header+ECC. Pinned explicitly
# rather than left to the library's default, because the two differ by a
# 16-byte offset into every sector - which does not fail, it just reads
# plausible-looking rubbish.
set(CHDR_WANT_RAW_DATA_SECTOR OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_SUBCODE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libchdr EXCLUDE_FROM_ALL)

add_library(retro3do_core STATIC
    src/core/arm60.cpp
    src/core/audio_ring.cpp
    src/core/bus.cpp
    src/core/chd.cpp
    src/core/clio.cpp
    src/core/console.cpp
    src/core/disc.cpp
    src/core/frame_mailbox.cpp
    src/core/madam.cpp
    src/core/pad.cpp
    src/core/pad_layout.cpp
    src/core/path.cpp
    src/core/settings.cpp
    src/core/sport.cpp
    src/core/vdlp.cpp
    src/core/xbus.cpp
)
target_include_directories(retro3do_core PUBLIC src)
target_link_libraries(retro3do_core PRIVATE chdr-static)

# The core is a software emulator: it is the one thing that must be optimised in
# every configuration, or a debug build is too slow to run a game at all. The
# app and UI stay fully debuggable.
if(NOT MSVC)
    target_compile_options(retro3do_core PRIVATE
        $<$<CONFIG:Debug>:-O2>
        -fno-math-errno
    )
    target_compile_options(retro3do_core PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

# ---------------------------------------------------------------------------
# SDL3
# ---------------------------------------------------------------------------
if(RETRO3DO_USE_SYSTEM_SDL)
    find_package(SDL3 REQUIRED CONFIG)
    set(RETRO3DO_SDL_TARGET SDL3::SDL3)
else()
    # Android is the exception: its Java side loads libSDL3.so by name before it
    # loads ours, so SDL has to be shared there. Everywhere else a static SDL
    # keeps the app a single file. The Android CMakeLists sets these first, so
    # only fill in a default when nothing has chosen yet.
    if(ANDROID)
        set(SDL_SHARED ON  CACHE BOOL "")
        set(SDL_STATIC OFF CACHE BOOL "")
    else()
        set(SDL_SHARED OFF CACHE BOOL "")
        set(SDL_STATIC ON  CACHE BOOL "")
    endif()
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

    if(TARGET SDL3::SDL3-static)
        set(RETRO3DO_SDL_TARGET SDL3::SDL3-static)
    else()
        set(RETRO3DO_SDL_TARGET SDL3::SDL3-shared)
    endif()
endif()

# ---------------------------------------------------------------------------
# Dear ImGui, with the SDL3 renderer backend.
#
# The SDL_Renderer backend is chosen over a graphics-API-specific one on
# purpose: SDL_Renderer already resolves to GL, GLES, Metal, Vulkan or D3D per
# platform, so Android and iOS need no separate render path here.
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})
target_link_libraries(imgui PUBLIC ${RETRO3DO_SDL_TARGET})

# ---------------------------------------------------------------------------
# The application
# ---------------------------------------------------------------------------
set(RETRO3DO_APP_SOURCES
    src/main.cpp
    src/platform/app.cpp
    src/platform/emulator_thread.cpp
    src/ui/ui.cpp
    src/ui/file_browser.cpp
    src/ui/touch_pad.cpp
    src/platform/storage.cpp
    src/platform/android_storage.cpp
)

if(ANDROID)
    # SDLActivity dlopens the app rather than executing it, so on Android the
    # app is a shared library and must be called "main".
    add_library(retro3do SHARED ${RETRO3DO_APP_SOURCES})
    set_target_properties(retro3do PROPERTIES OUTPUT_NAME "main")
else()
    add_executable(retro3do ${RETRO3DO_APP_SOURCES})
endif()

find_package(Threads REQUIRED)
target_link_libraries(retro3do PRIVATE retro3do_core imgui ${RETRO3DO_SDL_TARGET} Threads::Threads)
target_include_directories(retro3do PRIVATE src)

if(IOS)
    # An iOS build is an app bundle, not a bare executable. Signing is left off
    # here on purpose: CI has no identity to give, and the archive step signs
    # the bundle with the app's own certificate anyway.
    set_target_properties(retro3do PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "Retro-3DO"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.crownparkcomputing.retro3do"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
    )
endif()

# ---------------------------------------------------------------------------
# Tests
#
# These run on the host with no window and no device. Per the project's testing
# rule, a bug found on hardware gets reproduced here first.
# ---------------------------------------------------------------------------
if(RETRO3DO_BUILD_TESTS)
    enable_testing()
    add_executable(retro3do_tests
        tests/test_main.cpp
        tests/test_arm60.cpp
        tests/test_audio_pad.cpp
        tests/test_bus.cpp
        tests/test_chd.cpp
        tests/test_clio.cpp
        tests/test_disc.cpp
        tests/test_frame_mailbox.cpp
        tests/test_madam.cpp
        tests/test_pad_layout.cpp
        tests/test_path.cpp
        tests/test_settings.cpp
        tests/test_sport.cpp
        tests/test_vdlp.cpp
    )
    target_link_libraries(retro3do_tests PRIVATE retro3do_core)
    find_package(Threads REQUIRED)
    target_link_libraries(retro3do_tests PRIVATE Threads::Threads)
    target_include_directories(retro3do_tests PRIVATE src tests)
    add_test(NAME retro3do_tests COMMAND retro3do_tests)
endif()
