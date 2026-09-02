# Android entry point into the shared CMake tree.
#
# Gradle points its externalNativeBuild at this file; everything it builds comes
# from the project root, so there is no Android-specific copy of the emulator to
# keep in step with the desktop one.
cmake_minimum_required(VERSION 3.21)
project(Retro3DOAndroid)

# SDL's Android backend expects the app to be a shared library named "main",
# which SDLActivity loads. The root CMakeLists already renames the target when
# ANDROID is set.
set(RETRO3DO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RETRO3DO_USE_SYSTEM_SDL OFF CACHE BOOL "" FORCE)

# SDL on Android must be a shared library: the Java side loads libSDL3.so by
# name before it loads ours.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../.. ${CMAKE_CURRENT_BINARY_DIR}/retro3do)
