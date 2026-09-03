#pragma once

// Which kind of machine this build is for.
//
// This exists because __APPLE__ answers the wrong question. It is defined for
// iPhone, iPad and Mac alike, and the app was using it to decide whether to
// take over the whole screen and draw an on-screen pad. That is right on a
// phone and wrong on a Mac, where it would give a Mac user a fullscreen window
// with a touch d-pad painted over it and no way to resize.
//
// Ask about the FORM of the device instead: is there a touchscreen and no
// window manager (a handheld), or is there a desktop with a pointer and real
// windows. Apple's TargetConditionals is the only place that distinction is
// available, and it needs a header - the preprocessor alone cannot tell iOS
// from macOS.

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
// iPhone, iPad, the simulator, and Mac Catalyst.
#define RETRO3DO_PLATFORM_IOS 1
#else
#define RETRO3DO_PLATFORM_IOS 0
#endif

#if defined(__APPLE__) && !RETRO3DO_PLATFORM_IOS
#define RETRO3DO_PLATFORM_MACOS 1
#else
#define RETRO3DO_PLATFORM_MACOS 0
#endif

#if defined(__ANDROID__)
#define RETRO3DO_PLATFORM_ANDROID 1
#else
#define RETRO3DO_PLATFORM_ANDROID 0
#endif

// A handheld: the app owns the screen, input arrives as touches, and there is
// no window to size or move.
#if RETRO3DO_PLATFORM_ANDROID || RETRO3DO_PLATFORM_IOS
#define RETRO3DO_MOBILE 1
#else
#define RETRO3DO_MOBILE 0
#endif

// A desktop: windows, a pointer, a keyboard, and a file system the user can
// reach without a document picker.
#define RETRO3DO_DESKTOP (!RETRO3DO_MOBILE)
