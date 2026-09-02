#include "app.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>

#include "core/console.h"
#include "core/frame_mailbox.h"
#include "core/pad.h"
#include "core/settings.h"
#include "platform/android_storage.h"
#include "platform/storage.h"
#include "ui/touch_pad.h"
#include "platform/emulator_thread.h"
#include "ui/ui.h"

namespace retro3do {
namespace {

#if defined(__ANDROID__)
extern "C" bool retro3do_configure_vulkan_driver(
    const char* private_directory, const char* native_library_directory,
    const char* driver_directory, const char* driver_library);
#endif

constexpr int kDefaultWindowWidth  = 1280;
constexpr int kDefaultWindowHeight = 720;

// One NVRAM, shared by every title, exactly as a console has one. Keeping a
// separate image per disc would be tidier to reason about, but it is not what
// the hardware does and it would break the titles that read each other's saves.
constexpr const char* kNvramFile = "nvram.bin";

std::string library_key(int index, const std::string& field) {
    return "library." + std::to_string(index) + "." + field;
}

std::string lowercased(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool has_disc_extension(const std::string& name) {
    const std::string lower = lowercased(name);
    static constexpr const char* kExtensions[] = {
        ".chd", ".iso", ".bin", ".cue", ".img"
    };
    for (const char* extension : kExtensions) {
        const size_t length = std::char_traits<char>::length(extension);
        if (lower.size() > length &&
            lower.compare(lower.size() - length, length, extension) == 0) {
            return true;
        }
    }
    return false;
}

bool has_bios_extension(const std::string& name, bool allow_bin) {
    const std::string lower = lowercased(name);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".rom") == 0) {
        return true;
    }
    return allow_bin && lower.size() > 4 &&
           lower.compare(lower.size() - 4, 4, ".bin") == 0;
}

}  // namespace

App::App() = default;

App::~App() {
    if (lifecycle_watch_registered_) {
        SDL_RemoveEventWatch(&App::lifecycle_event_watch, this);
        lifecycle_watch_registered_ = false;
    }
    // Stop emulating before anything it touches goes away.
    if (settings_ && touch_ && console_) {
        save_settings();
    }
    // Last chance to write out a save the user made just before quitting.
    if (emulator_) {
        emulator_->stop();
        save_nvram();
    }
    emulator_.reset();
    ui_.reset();
    stop_audio();
    for (SDL_Gamepad*& pad : gamepads_) {
        if (pad != nullptr) {
            SDL_CloseGamepad(pad);
            pad = nullptr;
        }
    }
    if (frame_texture_) SDL_DestroyTexture(frame_texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool App::init() {
    // SDL_INIT_AUDIO is required or SDL_OpenAudioDeviceStream fails with
    // "Audio subsystem is not initialized" - which is reported as "audio
    // unavailable" and looks like a device problem rather than a missing flag.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        last_error_ = std::string("SDL could not start: ") + SDL_GetError();
        return false;
    }
    lifecycle_watch_registered_ =
        SDL_AddEventWatch(&App::lifecycle_event_watch, this);

    // Renderer selection is a start-up decision, so read just the flat
    // settings file before a window exists. load_settings() runs later once
    // the UI and machine are available and fills in everything else.
    settings_ = std::make_unique<Settings>();
    settings_->load(Storage::join(Storage::writable_directory(), "settings.cfg"));

    const std::string requested_renderer =
        settings_->get(settings_key::kRendererBackend, "auto");
    std::string render_hint = requested_renderer == "vulkan" ? "vulkan" : "";

#if defined(__ANDROID__)
    const std::string driver_directory =
        settings_->get(settings_key::kGpuDriverDirectory);
    const std::string driver_library =
        settings_->get(settings_key::kGpuDriverLibrary);
    const bool custom_driver =
        !driver_directory.empty() && !driver_library.empty() &&
        Storage::exists(Storage::join(driver_directory, driver_library));
    if (custom_driver) {
        const std::string native_libraries =
            AndroidStorage::native_library_directory();
        const std::string private_storage = Storage::writable_directory();
        if (retro3do_configure_vulkan_driver(
                private_storage.c_str(), native_libraries.c_str(),
                driver_directory.c_str(), driver_library.c_str())) {
            SDL_SetHint(SDL_HINT_VULKAN_LIBRARY,
                        Storage::join(native_libraries,
                                      "libretro3do_vulkan_loader.so").c_str());
            render_hint = "vulkan";
        } else {
            renderer_startup_message_ =
                "Custom driver unavailable; using the system renderer";
        }
    } else if (!driver_directory.empty() || !driver_library.empty()) {
        renderer_startup_message_ =
            "Imported driver files are missing; using the system renderer";
    }
#endif

    if (!render_hint.empty()) SDL_SetHint(SDL_HINT_RENDER_DRIVER, render_hint.c_str());

    // On a phone or tablet there is no window to size — SDL gives us the
    // display. Asking for a resizable window is harmless there and correct on
    // desktop.
    // Fullscreen on a handheld, a window on a desktop. Without this the system
    // status bar sits on top of the app: not merely untidy, it covers the top
    // row of controls and steals touches meant for them.
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__ANDROID__) || defined(__APPLE__)
    flags |= SDL_WINDOW_FULLSCREEN;
#elif !defined(__linux__)
    flags |= SDL_WINDOW_RESIZABLE;
#endif

    if (!SDL_CreateWindowAndRenderer("Retro-3DO", kDefaultWindowWidth,
                                     kDefaultWindowHeight, flags, &window_,
                                     &renderer_)) {
        const std::string requested_error = SDL_GetError();
        if (!render_hint.empty()) {
            // A bad third-party driver must never strand the user outside the
            // app. Fall back to SDL's platform choice and report it on System.
            SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
            SDL_ResetHint(SDL_HINT_VULKAN_LIBRARY);
            if (SDL_CreateWindowAndRenderer("Retro-3DO", kDefaultWindowWidth,
                                            kDefaultWindowHeight, flags,
                                            &window_, &renderer_)) {
                renderer_startup_message_ =
                    "Vulkan failed; using automatic renderer (" +
                    requested_error + ")";
            }
        }
        if (renderer_ == nullptr) {
            last_error_ = std::string("Could not create a window: ") + SDL_GetError();
            return false;
        }
    }
    actual_renderer_ = SDL_GetRendererName(renderer_) != nullptr
                           ? SDL_GetRendererName(renderer_)
                           : "unknown";
    SDL_Log("Renderer: %s", actual_renderer_.c_str());

#if defined(__linux__)
    // The launcher is deliberately a fixed landscape surface on Linux. A
    // tiling compositor otherwise squeezes the window until the A-Z strip is
    // unreadable or clipped, even though the app requested a useful initial
    // size. Equal minimum and maximum bounds make 1280x720 authoritative.
    SDL_SetWindowMinimumSize(window_, kDefaultWindowWidth, kDefaultWindowHeight);
    SDL_SetWindowMaximumSize(window_, kDefaultWindowWidth, kDefaultWindowHeight);
#endif

    // Synchronise presentation to the panel. Emulation is paced independently
    // on EmulatorThread, so blocking this UI thread at vblank cannot change the
    // machine's speed. Leaving vsync disabled made Android submit the same
    // texture hundreds of times per second and produced visible horizontal
    // tearing whenever a game panned laterally.
    if (!SDL_SetRenderVSync(renderer_, 1)) {
        SDL_Log("Could not enable presentation vsync: %s", SDL_GetError());
    }

    console_ = std::make_unique<Console>();
    mailbox_ = std::make_unique<FrameMailbox>();
    touch_ = std::make_unique<TouchPad>();
    emulator_ = std::make_unique<EmulatorThread>(*console_, *mailbox_);

    ui_ = std::make_unique<Ui>();
    if (!ui_->init(window_, renderer_)) {
        last_error_ = "Could not start the user interface";
        return false;
    }
    ui_->set_renderer_status(actual_renderer_, renderer_startup_message_);

    start_audio();

    // Any pad already plugged in when the app starts does not generate a
    // connection event, so they are opened explicitly.
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids != nullptr) {
        for (int i = 0; i < count; ++i) {
            open_gamepad(ids[i]);
        }
        SDL_free(ids);
    }

    // Emulation runs on its own thread from here on. It is started paused, so
    // nothing runs until there is something to run.
    {
        // Lay the pad out for the screen we actually have before settings are
        // applied, so a first run gets correct geometry rather than the
        // constructor's placeholder.
        int window_w = 0;
        int window_h = 0;
        SDL_GetRenderOutputSize(renderer_, &window_w, &window_h);
        touch_->reset_layout(window_w, window_h);
    }
    load_settings();
    load_nvram();

    if (AndroidStorage::available()) {
        ui_->set_retro_media_status("", 0, 0, false, true,
                                    "Checking account...");
        AndroidStorage::begin_retro_media_status();
    }

    emulator_->set_paused(!start_on_launch_);
    emulating_ = start_on_launch_;
    if (start_on_launch_) {
        ui_->hide_launcher();
    }
    emulator_->start();

    running_ = true;
    return true;
}

bool App::lifecycle_event_watch(void* userdata, SDL_Event* event) {
    auto* app = static_cast<App*>(userdata);
    switch (event->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            // SDL's Android pump blocks inside the lifecycle transition and
            // does not return to tick() until the app is foregrounded again.
            // Release the Vulkan swapchain here, on SDL's own thread, before
            // Java destroys the native Surface and before SDL emits RESTORED.
            app->backgrounded_.store(true, std::memory_order_release);
            app->suspend_renderer();
            break;
        case SDL_EVENT_DID_ENTER_BACKGROUND:
            app->backgrounded_.store(true, std::memory_order_release);
            break;
        case SDL_EVENT_WILL_ENTER_FOREGROUND:
            // The Android surface has not been recreated yet. Keep rendering
            // suspended until DID_ENTER_FOREGROUND and a short driver settle
            // period have both passed.
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            app->render_resume_after_ms_.store(SDL_GetTicks() + 500u,
                                               std::memory_order_release);
            app->backgrounded_.store(false, std::memory_order_release);
            break;
        default:
            break;
    }
    return true;
}

void App::suspend_renderer() {
    if (renderer_suspended_ || renderer_ == nullptr) return;

    // Android owns the native window surface and destroys it while the app is
    // in the background. Vulkan swapchain objects must go away before that
    // surface is reused; retaining SDL's renderer makes the first Present on
    // return walk stale driver state on some Adreno/Turnip combinations.
    if (ui_) ui_->suspend_renderer();
    if (frame_texture_ != nullptr) {
        SDL_DestroyTexture(frame_texture_);
        frame_texture_ = nullptr;
    }
    texture_width_ = 0;
    texture_height_ = 0;
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
    renderer_suspended_ = true;
}

bool App::resume_renderer() {
    if (!renderer_suspended_) return renderer_ != nullptr;

    const char* preferred = actual_renderer_.empty()
                                ? nullptr
                                : actual_renderer_.c_str();
    renderer_ = SDL_CreateRenderer(window_, preferred);
    if (renderer_ == nullptr) {
        const std::string preferred_error = SDL_GetError();
        // A custom Vulkan driver may not survive a system surface transition.
        // Automatic selection keeps the session usable instead of crashing.
        SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
        SDL_ResetHint(SDL_HINT_VULKAN_LIBRARY);
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            last_error_ = "Could not restore the Android renderer: " +
                          preferred_error + "; fallback: " + SDL_GetError();
            return false;
        }
        renderer_startup_message_ =
            "Vulkan could not resume; using the system renderer";
    }

    actual_renderer_ = SDL_GetRendererName(renderer_) != nullptr
                           ? SDL_GetRendererName(renderer_)
                           : "unknown";
    if (!SDL_SetRenderVSync(renderer_, 1)) {
        SDL_Log("Could not restore presentation vsync: %s", SDL_GetError());
    }
    if (ui_ && !ui_->resume_renderer(renderer_)) {
        last_error_ = "Could not restore the user interface renderer";
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
        return false;
    }
    if (ui_) {
        ui_->set_renderer_status(actual_renderer_, renderer_startup_message_);
    }
    renderer_suspended_ = false;
    return true;
}

void App::ensure_frame_texture(int width, int height) {
    if (frame_texture_ && texture_width_ == width && texture_height_ == height) {
        return;
    }
    if (frame_texture_) {
        SDL_DestroyTexture(frame_texture_);
        frame_texture_ = nullptr;
    }

    frame_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, width, height);
    if (frame_texture_ != nullptr) {
        // The VDLP has already produced the hardware's interpolated 640x480
        // signal. Linear sampling preserves that graded output when the final
        // display size is not an integer multiple of it.
        SDL_SetTextureScaleMode(frame_texture_, SDL_SCALEMODE_LINEAR);
        texture_width_  = width;
        texture_height_ = height;
    }
}

void App::handle_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ui_->process_event(event);

        // Touch controls get first refusal, but only when the UI is not using
        // the pointer: otherwise a tap on a menu would also press a button
        // hiding underneath it.
        if (renderer_ != nullptr && !ui_->wants_mouse()) {
            int window_w = 0;
            int window_h = 0;
            SDL_GetRenderOutputSize(renderer_, &window_w, &window_h);
            if (touch_->handle_event(event, window_w, window_h, console_->pads())) {
                continue;
            }
        }

        switch (event.type) {
            case SDL_EVENT_QUIT:
                running_ = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (event.window.windowID == SDL_GetWindowID(window_)) {
                    running_ = false;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                // A panel with focus gets the keys; otherwise they are the pad.
                if (!ui_->wants_keyboard()) {
                    apply_keyboard(event.key.scancode,
                                   event.type == SDL_EVENT_KEY_DOWN);
                }
                break;

            case SDL_EVENT_GAMEPAD_ADDED:
                open_gamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                close_gamepad(event.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                const int slot = pad_slot_for(event.gbutton.which);
                if (slot >= 0) {
                    apply_gamepad_button(slot, event.gbutton.button,
                                         event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                }
                break;
            }

            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                const int slot = pad_slot_for(event.gaxis.which);
                if (slot >= 0) {
                    apply_gamepad_axis(slot, event.gaxis.axis, event.gaxis.value);
                }
                break;
            }

            default:
                break;
        }
    }
}

void App::present() {
    const int width = mailbox_->width();
    const int height = mailbox_->height();
    if (width <= 0 || height <= 0) {
        return;
    }

    ensure_frame_texture(width, height);
    if (frame_texture_ == nullptr) {
        return;
    }

    // Only re-upload when a new frame has actually arrived. If the display is
    // running faster than the emulator - which it is, at 60 Hz against a paused
    // or slow machine - the same texture is simply drawn again.
    const u32* pixels = mailbox_->acquire();
    if (pixels != nullptr) {
        SDL_UpdateTexture(frame_texture_, nullptr, pixels,
                          width * static_cast<int>(sizeof(u32)));
    }

    // Fit to the selected presentation shape. 4:3 preserves the machine's
    // original geometry; 16:9 deliberately fills a modern handheld screen.
    int window_w = 0;
    int window_h = 0;
    SDL_GetRenderOutputSize(renderer_, &window_w, &window_h);

    const bool bezel = ui_ != nullptr && ui_->bezel();
    const bool crt = ui_ != nullptr && ui_->crt_effect();
    const float target_aspect =
        ui_ != nullptr && ui_->widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
    (void)height;
    const float bezel_margin =
        bezel ? static_cast<float>(std::min(window_w, window_h)) * 0.035f : 0.0f;
    float draw_w = static_cast<float>(window_w) - bezel_margin * 2.0f;
    float draw_h = draw_w / target_aspect;
    const float available_h = static_cast<float>(window_h) - bezel_margin * 2.0f;
    if (draw_h > available_h) {
        draw_h = available_h;
        draw_w = draw_h * target_aspect;
    }

    SDL_FRect destination;
    destination.x = (static_cast<float>(window_w) - draw_w) * 0.5f;
    destination.y = (static_cast<float>(window_h) - draw_h) * 0.5f;
    destination.w = draw_w;
    destination.h = draw_h;

    if (bezel) {
        const float rim = std::max(5.0f, bezel_margin * 0.72f);
        SDL_FRect surround{destination.x - rim, destination.y - rim,
                           destination.w + rim * 2.0f,
                           destination.h + rim * 2.0f};
        SDL_SetRenderDrawColor(renderer_, 25, 29, 36, 255);
        SDL_RenderFillRect(renderer_, &surround);
        SDL_SetRenderDrawColor(renderer_, 71, 78, 88, 255);
        SDL_RenderRect(renderer_, &surround);
        SDL_FRect inner{destination.x - 2.0f, destination.y - 2.0f,
                        destination.w + 4.0f, destination.h + 4.0f};
        SDL_SetRenderDrawColor(renderer_, 3, 4, 6, 255);
        SDL_RenderRect(renderer_, &inner);
    }

    SDL_RenderTexture(renderer_, frame_texture_, nullptr, &destination);

    if (crt) {
        // Renderer-independent CRT treatment. Keeping it in SDL primitives
        // gives identical controls on Vulkan, GLES, Metal and desktop GL and
        // avoids making a custom shader a requirement for correct emulation.
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 38);
        const float bottom = destination.y + destination.h;
        const float right = destination.x + destination.w;
        for (float y = destination.y + 1.0f; y < bottom; y += 3.0f) {
            SDL_RenderLine(renderer_, destination.x, y, right, y);
        }
        // A restrained dark edge suggests the falloff of a tube without
        // cropping game pixels or bending HUD text.
        for (int edge = 0; edge < 4; ++edge) {
            const float inset = static_cast<float>(edge);
            SDL_FRect shade{destination.x + inset, destination.y + inset,
                            destination.w - inset * 2.0f,
                            destination.h - inset * 2.0f};
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0,
                                   static_cast<Uint8>(70 - edge * 12));
            SDL_RenderRect(renderer_, &shade);
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    }
}


// ---------------------------------------------------------------------------
// Opening files
// ---------------------------------------------------------------------------
//
// A path and a document URI take different routes but mean the same thing to
// everyone upstream, so the choice is made once, here.
bool App::open_bios(const std::string& path, const std::string& name) {
    if (path.empty()) return false;
    // Logged because the single most likely failure is a display name reaching
    // here instead of a document URI, and that is invisible without seeing the
    // actual string.
    SDL_Log("open BIOS: %s", path.c_str());
    if (AndroidStorage::available()) {
        return console_->load_bios_fd(AndroidStorage::open_document(path),
                                      name.empty() ? path : name);
    }
    return console_->load_bios(path);
}

bool App::open_disc(const std::string& path, const std::string& name) {
    if (path.empty()) return false;
    SDL_Log("open disc: %s", path.c_str());
    if (AndroidStorage::available()) {
        return console_->load_disc_fd(AndroidStorage::open_document(path),
                                      name.empty() ? path : name);
    }
    return console_->load_disc(path);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
void App::set_launch_files(std::string bios, std::string disc) {
    launch_bios_ = std::move(bios);
    launch_disc_ = std::move(disc);
}

// The NVRAM is restored before emulation starts, so a title that reads it on
// its first frame sees the user's saves rather than an empty card.
//
// A missing file is a first run, not an error: the machine has already put a
// formatted image there. A file of the wrong length is refused rather than
// padded - a half-restored NVRAM would look to a title like a corrupt one, and
// it would offer to reformat it, which would lose everything.
void App::load_nvram() {
    if (!console_) return;
    const std::string path = Storage::join(Storage::writable_directory(), kNvramFile);
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (data == nullptr) {
        return;
    }
    console_->bus().restore_nvram(static_cast<const u8*>(data), size);
    SDL_free(data);
}

// Called every turn of the display loop. It costs a lock and a bool nearly
// every time: the machine only writes the NVRAM when the user saves, so the
// file is only touched then too.
void App::save_nvram() {
    if (!emulator_) return;
    if (!emulator_->take_nvram(nvram_scratch_)) {
        return;
    }
    const std::string path = Storage::join(Storage::writable_directory(), kNvramFile);
    SDL_SaveFile(path.c_str(), nvram_scratch_.data(), nvram_scratch_.size());
}

void App::load_settings() {
    settings_->load(Storage::join(Storage::writable_directory(), "settings.cfg"));

    load_library();

    const std::string bios_folder =
        settings_->get(settings_key::kBiosFolderPath);
    const std::string games_folder =
        settings_->get(settings_key::kGamesFolderPath);
    ui_->configure_setup(
        !AndroidStorage::available() ||
            settings_->get_bool(settings_key::kSetupComplete, false),
        settings_->get(settings_key::kBiosFolderName), bios_folder,
        settings_->get(settings_key::kGamesFolderName), games_folder);

    console_->set_region(settings_->get_int(settings_key::kRegion, 0) == 1
                             ? Region::Pal
                             : Region::Ntsc);
    const int cpu_scale = settings_->get_int(settings_key::kCpuScale, 100);
    console_->set_cpu_scale_percent(cpu_scale == 125 || cpu_scale == 150
                                        ? static_cast<u32>(cpu_scale)
                                        : 100u);
    touch_->load_layout(*settings_);
    ui_->set_performance_visible(
        settings_->get_bool(settings_key::kPerformanceHud, false));
    ui_->configure_video(
        settings_->get(settings_key::kRendererBackend, "auto") == "vulkan" ? 1
                                                                              : 0,
        settings_->get_bool(settings_key::kAspectWidescreen, false),
        settings_->get_bool(settings_key::kBezel, false),
        settings_->get_bool(settings_key::kCrtEffect, false),
        settings_->get(settings_key::kGpuDriverName));
    ui_->set_renderer_status(actual_renderer_, renderer_startup_message_);
    ui_->configure_retro_media(
        settings_->get(settings_key::kRetroMediaCardType, "box2d"));
    ui_->set_retro_media_email_hint(
        AndroidStorage::retro_media_saved_email());
    load_retro_media_artwork();

    // Remembered files are re-opened, but a failure is not reported as an
    // error: a SAF grant can be revoked and an iOS container is reassigned on
    // every install, so yesterday's path being gone is ordinary rather than
    // exceptional. It simply is not loaded, and the launcher shows why.
    const std::string bios = launch_bios_.empty()
                                 ? settings_->get(settings_key::kBiosPath)
                                 : launch_bios_;
    ui_->set_remembered_bios(settings_->get(settings_key::kBiosName), bios);
    if (!bios.empty()) {
        if (open_bios(bios, launch_bios_.empty()
                                ? settings_->get(settings_key::kBiosName)
                                : Storage::base_name(launch_bios_))) {
            SDL_Log("Reopened BIOS from settings");
            // Explicit command-line launches remain scriptable. A normal app
            // launch now opens the game library, which is the useful home
            // screen once more than one title has been added.
            start_on_launch_ = !launch_bios_.empty() || !launch_disc_.empty();
        } else {
            SDL_Log("Remembered BIOS is no longer reachable; forgetting it");
            settings_->remove(settings_key::kBiosPath);
            settings_->remove(settings_key::kBiosName);
        }
    }
    const std::string remembered_bios_name =
        lowercased(settings_->get(settings_key::kBiosName));
    const bool arcade_bios = remembered_bios_name.find("arcade") != std::string::npos ||
                             remembered_bios_name.find("saot") != std::string::npos;
    if (AndroidStorage::available() && (!console_->bios_loaded() || arcade_bios) &&
        !bios_folder.empty()) {
        use_bios_folder(settings_->get(settings_key::kBiosFolderName),
                        bios_folder);
    }

    const std::string disc = launch_disc_.empty()
                                 ? settings_->get(settings_key::kDiscPath)
                                 : launch_disc_;
    ui_->set_remembered_disc(settings_->get(settings_key::kDiscName), disc);
    if (!disc.empty()) {
        const std::string disc_name = launch_disc_.empty()
                                          ? settings_->get(settings_key::kDiscName)
                                          : Storage::base_name(launch_disc_);
        if (!open_disc(disc, disc_name)) {
            settings_->remove(settings_key::kDiscPath);
            settings_->remove(settings_key::kDiscName);
        } else {
            // Android's configured games folder owns its library. Desktop has
            // no SAF setup, so direct opens remain its way to populate cards.
            if (!AndroidStorage::available()) remember_game(disc_name, disc);
        }
    }
    if (AndroidStorage::available() && library_.games().empty() &&
        !games_folder.empty()) {
        scan_games_folder(settings_->get(settings_key::kGamesFolderName),
                          games_folder);
    }
}

void App::load_library() {
    persisted_library_count_ =
        std::clamp(settings_->get_int(settings_key::kLibraryCount, 0), 0, 512);
    std::vector<LibraryGame> games;
    games.reserve(static_cast<size_t>(persisted_library_count_));
    for (int i = 0; i < persisted_library_count_; ++i) {
        const std::string target = settings_->get(library_key(i, "target"));
        if (!target.empty()) {
            LibraryGame game;
            game.name = settings_->get(library_key(i, "name"));
            game.target = target;
            const int disc_count = std::clamp(
                settings_->get_int(library_key(i, "disc_count"), 0), 0, 32);
            for (int disc = 0; disc < disc_count; ++disc) {
                const std::string prefix = "disc_" + std::to_string(disc) + "_";
                const std::string disc_target =
                    settings_->get(library_key(i, prefix + "target"));
                if (disc_target.empty()) continue;
                game.discs.push_back({
                    settings_->get(library_key(i, prefix + "name")),
                    disc_target,
                    settings_->get_int(library_key(i, prefix + "number"), disc + 1)});
            }
            games.push_back(std::move(game));
        }
    }
    library_.replace(games);
    ui_->set_game_library(library_.games());
}

void App::save_library() {
    const int count = static_cast<int>(library_.games().size());
    const int old_count = persisted_library_count_;
    for (int i = 0; i < std::max(old_count, count); ++i) {
        if (i < count) {
            settings_->set(library_key(i, "name"), library_.games()[i].name);
            settings_->set(library_key(i, "target"), library_.games()[i].target);
            const auto& discs = library_.games()[i].discs;
            const int old_discs = std::clamp(
                settings_->get_int(library_key(i, "disc_count"), 0), 0, 32);
            settings_->set_int(library_key(i, "disc_count"),
                               static_cast<int>(discs.size()));
            for (int disc = 0;
                 disc < std::max(old_discs, static_cast<int>(discs.size())); ++disc) {
                const std::string prefix = "disc_" + std::to_string(disc) + "_";
                if (disc < static_cast<int>(discs.size())) {
                    settings_->set(library_key(i, prefix + "name"),
                                   discs[disc].name);
                    settings_->set(library_key(i, prefix + "target"),
                                   discs[disc].target);
                    settings_->set_int(library_key(i, prefix + "number"),
                                       discs[disc].number);
                } else {
                    settings_->remove(library_key(i, prefix + "name"));
                    settings_->remove(library_key(i, prefix + "target"));
                    settings_->remove(library_key(i, prefix + "number"));
                }
            }
        } else {
            settings_->remove(library_key(i, "name"));
            settings_->remove(library_key(i, "target"));
            settings_->remove(library_key(i, "disc_count"));
        }
    }
    settings_->set_int(settings_key::kLibraryCount, count);
    persisted_library_count_ = count;
}

void App::load_retro_media_artwork() {
    if (!ui_) return;
    ui_->set_retro_media_artwork(
        AndroidStorage::retro_media_artwork(ui_->retro_media_type()));
}

void App::remember_game(const std::string& name, const std::string& target) {
    const std::string friendly = name.empty() ? Storage::base_name(target) : name;
    library_.add(friendly, target);
    ui_->set_game_library(library_.games());
}

void App::use_bios_folder(const std::string& name, const std::string& target) {
    if (target.empty()) return;
    settings_->set(settings_key::kBiosFolderPath, target);
    settings_->set(settings_key::kBiosFolderName, name);

    // A firmware folder commonly contains several regional and arcade ROMs.
    // Keep the already-working choice when it is in this folder; on a first
    // install prefer the well-supported retail Panasonic images.
    std::vector<DocumentEntry> candidates;
    std::vector<std::pair<std::string, int>> pending{{target, 0}};
    for (size_t cursor = 0; cursor < pending.size() && cursor < 256; ++cursor) {
        const auto [folder, depth] = pending[cursor];
        for (const DocumentEntry& entry : AndroidStorage::list(folder)) {
            if (entry.is_directory && depth < 6) {
                pending.emplace_back(entry.uri, depth + 1);
            } else if (!entry.is_directory) {
                if (has_bios_extension(entry.name, true)) candidates.push_back(entry);
            }
        }
    }
    const std::string remembered = settings_->get(settings_key::kBiosPath);
    auto preference = [&](const DocumentEntry& entry) {
        const std::string lower = lowercased(entry.name);
        const bool arcade = lower.find("arcade") != std::string::npos ||
                            lower.find("saot") != std::string::npos;
        if (!remembered.empty() && entry.uri == remembered && !arcade) return 0;
        if (lower == "panafz1.bin") return 1;
        if (lower == "panafz10.bin") return 2;
        if (lower.find("panafz") != std::string::npos) return 3;
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".rom") == 0) {
            return 4;
        }
        if (lower.find("goldstar") != std::string::npos) return 5;
        return 10;
    };
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&](const DocumentEntry& a, const DocumentEntry& b) {
                         return preference(a) < preference(b);
                     });
    for (const DocumentEntry& candidate : candidates) {
        if (!open_bios(candidate.uri, candidate.name)) continue;
        settings_->set(settings_key::kBiosPath, candidate.uri);
        settings_->set(settings_key::kBiosName, candidate.name);
        ui_->set_remembered_bios(candidate.name, candidate.uri);
        SDL_Log("BIOS found by setup: %s", candidate.name.c_str());
        break;
    }
    save_settings();
}

void App::scan_games_folder(const std::string& name, const std::string& target) {
    if (target.empty()) return;
    settings_->set(settings_key::kGamesFolderPath, target);
    settings_->set(settings_key::kGamesFolderName, name);

    // The selected folder is authoritative. Rebuilding means deleted or moved
    // images disappear instead of leaving dead cards behind.
    library_.replace({});
    std::vector<std::pair<std::string, int>> pending{{target, 0}};
    size_t added = 0;
    for (size_t cursor = 0; cursor < pending.size() && cursor < 1024 && added < 2048;
         ++cursor) {
        const auto [folder, depth] = pending[cursor];
        const std::vector<DocumentEntry> entries = AndroidStorage::list(folder);
        const bool folder_has_cue = std::any_of(
            entries.begin(), entries.end(), [](const DocumentEntry& entry) {
                const std::string lower = lowercased(entry.name);
                return !entry.is_directory && lower.size() > 4 &&
                       lower.compare(lower.size() - 4, 4, ".cue") == 0;
            });
        for (const DocumentEntry& entry : entries) {
            if (entry.is_directory) {
                if (depth < 8) pending.emplace_back(entry.uri, depth + 1);
                continue;
            }
            if (!has_disc_extension(entry.name)) continue;
            const std::string lower = lowercased(entry.name);
            if (folder_has_cue && lower.size() > 4 &&
                lower.compare(lower.size() - 4, 4, ".bin") == 0) {
                continue;  // the CUE is the game; its BIN files are tracks
            }
            if (library_.add(entry.name, entry.uri)) ++added;
            if (added >= 2048) break;
        }
    }
    ui_->set_game_library(library_.games());
    save_settings();
    SDL_Log("Setup library scan added %zu game%s", added, added == 1 ? "" : "s");
}

void App::save_settings() {
    settings_->set_int(settings_key::kRegion,
                       console_->region() == Region::Pal ? 1 : 0);
    settings_->set_int(settings_key::kCpuScale,
                       static_cast<int>(console_->cpu_scale_percent()));
    touch_->save_layout(*settings_);
    settings_->set_bool(settings_key::kPerformanceHud,
                        ui_->performance_visible());
    settings_->set(settings_key::kRendererBackend,
                   ui_->renderer_backend() == 1 ? "vulkan" : "auto");
    settings_->set_bool(settings_key::kAspectWidescreen, ui_->widescreen());
    settings_->set_bool(settings_key::kBezel, ui_->bezel());
    settings_->set_bool(settings_key::kCrtEffect, ui_->crt_effect());
    settings_->set(settings_key::kRetroMediaCardType,
                   ui_->retro_media_type());
    save_library();
    settings_->save();
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
bool App::start_audio() {
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = static_cast<int>(kAudioSampleRate);

    // A stream rather than a callback: SDL does the resampling if the device
    // will not take 44.1 kHz, which on a phone it frequently will not.
    audio_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                              &spec, nullptr, nullptr);
    if (audio_stream_ == nullptr) {
        // Audio not being available is not fatal. A device with no output, or
        // one whose audio server is busy, should still run the emulator.
        SDL_Log("Audio unavailable, continuing silently: %s", SDL_GetError());
        return false;
    }
    // Prime the stream before starting it. Feeding invented silence to reach
    // the target made startup and mode changes appear as audio underruns.
    audio_started_ = false;
    return true;
}

void App::stop_audio() {
    if (audio_stream_ != nullptr) {
        SDL_DestroyAudioStream(audio_stream_);
        audio_stream_ = nullptr;
    }
    audio_started_ = false;
}

void App::feed_audio() {
    if (audio_stream_ == nullptr) {
        return;
    }

    // Keep roughly two frames queued. Less and any hitch is audible; more and
    // input starts to feel detached from the sound.
    constexpr int kTargetQueuedBytes =
        static_cast<int>(kAudioSampleRate / 30) * static_cast<int>(sizeof(StereoSample));

    int queued = SDL_GetAudioStreamQueued(audio_stream_);
    if (queued >= kTargetQueuedBytes) {
        return;
    }

    StereoSample chunk[1024];
    int wanted = (kTargetQueuedBytes - queued) / static_cast<int>(sizeof(StereoSample));
    const int available = static_cast<int>(console_->audio().available());
    if (!audio_started_ && available < wanted) {
        return;
    }
    if (wanted > available) {
        wanted = available;
    }
    while (wanted > 0) {
        const u32 count = static_cast<u32>(wanted > 1024 ? 1024 : wanted);
        console_->audio().pull(chunk, count);
        SDL_PutAudioStreamData(audio_stream_, chunk,
                               static_cast<int>(count * sizeof(StereoSample)));
        wanted -= static_cast<int>(count);
    }
    if (!audio_started_) {
        SDL_ResumeAudioStreamDevice(audio_stream_);
        audio_started_ = true;
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void App::apply_keyboard(int scancode, bool down) {
    PadState& pads = console_->pads();
    switch (scancode) {
        case SDL_SCANCODE_UP:     pads.press(0, PadButton::Up, down); break;
        case SDL_SCANCODE_DOWN:   pads.press(0, PadButton::Down, down); break;
        case SDL_SCANCODE_LEFT:   pads.press(0, PadButton::Left, down); break;
        case SDL_SCANCODE_RIGHT:  pads.press(0, PadButton::Right, down); break;
        case SDL_SCANCODE_Z:      pads.press(0, PadButton::A, down); break;
        case SDL_SCANCODE_X:      pads.press(0, PadButton::B, down); break;
        case SDL_SCANCODE_C:      pads.press(0, PadButton::C, down); break;
        case SDL_SCANCODE_RETURN: pads.press(0, PadButton::Play, down); break;
        case SDL_SCANCODE_SPACE:  pads.press(0, PadButton::Stop, down); break;
        case SDL_SCANCODE_Q:      pads.press(0, PadButton::LeftShift, down); break;
        case SDL_SCANCODE_W:      pads.press(0, PadButton::RightShift, down); break;
        default: break;
    }
}

int App::pad_slot_for(u32 joystick_id) const {
    for (int slot = 0; slot < static_cast<int>(kMaxPads); ++slot) {
        if (gamepads_[slot] != nullptr &&
            SDL_GetGamepadID(gamepads_[slot]) == joystick_id) {
            return slot;
        }
    }
    return -1;
}

// A 3DO chains its pads: the machine sees one serial stream with every
// connected pad in it, which is why it needs no multitap and why four-player
// games exist for it at all. So a second controller is a second pad, not a
// second view of the first.
//
// A controller keeps its slot until it is unplugged. Compacting the list when
// player two leaves would hand player three's pad to player two mid-game.
void App::open_gamepad(u32 which) {
    if (pad_slot_for(which) >= 0) {
        return;
    }
    for (int slot = 0; slot < static_cast<int>(kMaxPads); ++slot) {
        if (gamepads_[slot] != nullptr) {
            continue;
        }
        SDL_Gamepad* pad = SDL_OpenGamepad(which);
        if (pad == nullptr) {
            return;
        }
        gamepads_[slot] = pad;
        axis_direction_[slot][0] = 0;
        axis_direction_[slot][1] = 0;
        console_->pads().set_connected(static_cast<u32>(slot), true);
        touch_->set_physical_gamepad_present(true);
        SDL_Log("Controller %s is 3DO pad %d", SDL_GetGamepadName(pad), slot + 1);
        return;
    }
}

void App::close_gamepad(u32 which) {
    const int slot = pad_slot_for(which);
    if (slot < 0) {
        return;
    }
    SDL_CloseGamepad(gamepads_[slot]);
    gamepads_[slot] = nullptr;

    // Release anything that was held when it went, or the game keeps running
    // in whatever direction the player was pushing.
    PadState& pads = console_->pads();
    for (u32 b = 0; b < static_cast<u32>(PadButton::Count); ++b) {
        pads.press(static_cast<u32>(slot), static_cast<PadButton>(b), false);
    }
    // Slot zero is always attached: it is what the keyboard and the on-screen
    // controls drive, and a machine with no pad in it is not a state worth
    // reproducing.
    if (slot != 0) {
        pads.set_connected(static_cast<u32>(slot), false);
    }

    bool any = false;
    for (SDL_Gamepad* g : gamepads_) {
        any = any || (g != nullptr);
    }
    touch_->set_physical_gamepad_present(any);
}

void App::apply_gamepad_button(int slot, int button, bool down) {
    PadState& pads = console_->pads();
    const u32 pad = static_cast<u32>(slot);
    switch (button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:    pads.press(pad, PadButton::Up, down); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  pads.press(pad, PadButton::Down, down); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  pads.press(pad, PadButton::Left, down); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: pads.press(pad, PadButton::Right, down); break;
        // The 3DO's three face buttons sit in a row, A B C from the left. On a
        // modern controller the bottom and right buttons are the two that fall
        // under the thumb, so they take A and B, and C goes to the left one -
        // which leaves the top button free rather than putting C somewhere it
        // cannot be reached in a hurry.
        case SDL_GAMEPAD_BUTTON_SOUTH:      pads.press(pad, PadButton::A, down); break;
        case SDL_GAMEPAD_BUTTON_EAST:       pads.press(pad, PadButton::B, down); break;
        case SDL_GAMEPAD_BUTTON_WEST:       pads.press(pad, PadButton::C, down); break;
        case SDL_GAMEPAD_BUTTON_START:      pads.press(pad, PadButton::Play, down); break;
        case SDL_GAMEPAD_BUTTON_BACK:       pads.press(pad, PadButton::Stop, down); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  pads.press(pad, PadButton::LeftShift, down); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: pads.press(pad, PadButton::RightShift, down); break;
        default: break;
    }
}

// An analogue stick standing in for a d-pad.
//
// The 3DO pad has no stick, so every direction a game can read is a switch -
// but most controllers people own now put the stick where the thumb goes and
// leave the d-pad as the awkward one. Without this a modern controller looks
// dead in a game that only reads directions.
//
// Two thresholds rather than one. A stick resting near a single threshold
// crosses it repeatedly on the smallest movement, and a direction that flickers
// on and off is worse than one that does not work: it walks a menu cursor away
// on its own. Pushing IN takes more than letting go does.
void App::apply_gamepad_axis(int slot, int axis, s16 value) {
    PadState& pads = console_->pads();
    const u32 pad = static_cast<u32>(slot);

    // The shoulder buttons are the 3DO's only shoulder controls, so a trigger
    // pulled counts as the same press. A trigger rests at zero rather than
    // centred, so it needs its own threshold and no direction at all.
    if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
        axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
        constexpr s16 kTriggerOn = 12000;
        const PadButton button = (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
                                     ? PadButton::LeftShift
                                     : PadButton::RightShift;
        pads.press(pad, button, value > kTriggerOn);
        return;
    }

    int index;
    PadButton negative;
    PadButton positive;
    if (axis == SDL_GAMEPAD_AXIS_LEFTX) {
        index = 0; negative = PadButton::Left; positive = PadButton::Right;
    } else if (axis == SDL_GAMEPAD_AXIS_LEFTY) {
        index = 1; negative = PadButton::Up; positive = PadButton::Down;
    } else {
        return;   // the right stick has nothing on this machine to drive
    }

    constexpr s16 kPushIn = 16000;    // about half deflection
    constexpr s16 kLetGo  = 9000;

    s8& held = axis_direction_[slot][index];
    s8 wanted = held;
    if (held == 0) {
        if (value >  kPushIn) wanted =  1;
        if (value < -kPushIn) wanted = -1;
    } else if (held > 0) {
        if (value < kLetGo) wanted = (value < -kPushIn) ? -1 : 0;
    } else {
        if (value > -kLetGo) wanted = (value > kPushIn) ? 1 : 0;
    }
    if (wanted == held) {
        return;
    }
    held = wanted;
    pads.press(pad, negative, wanted < 0);
    pads.press(pad, positive, wanted > 0);
}

void App::tick() {
    handle_events();

    // Android destroys or detaches the native surface while another app is in
    // front. Rendering even one more frame after WILL_ENTER_BACKGROUND can
    // make SDL_RenderPresent dereference that dead surface. These lifecycle
    // events are delivered only to an event watch, so the atomic flag is also
    // checked again immediately before presentation.
    const u64 resume_after =
        render_resume_after_ms_.load(std::memory_order_acquire);
    if (backgrounded_.load(std::memory_order_acquire) ||
        (resume_after != 0 && SDL_GetTicks() < resume_after)) {
        if (!lifecycle_pause_applied_) {
            emulator_->set_paused(true);
            lifecycle_pause_applied_ = true;
        }
        if (backgrounded_.load(std::memory_order_acquire)) suspend_renderer();
        SDL_Delay(16);
        return;
    }
    if (renderer_suspended_ && !resume_renderer()) {
        running_ = false;
        return;
    }
    render_resume_after_ms_.store(0, std::memory_order_release);
    if (lifecycle_pause_applied_) {
        emulator_->set_paused(!emulating_);
        lifecycle_pause_applied_ = false;
    }

    feed_audio();
    save_nvram();

    const GpuDriverImport imported_driver =
        AndroidStorage::consume_gpu_driver_import();
    if (imported_driver.ready) {
        if (imported_driver.success) {
            settings_->set(settings_key::kGpuDriverName, imported_driver.name);
            settings_->set(settings_key::kGpuDriverDirectory,
                           imported_driver.directory);
            settings_->set(settings_key::kGpuDriverLibrary,
                           imported_driver.library);
            ui_->configure_video(1, ui_->widescreen(), ui_->bezel(),
                                 ui_->crt_effect(), imported_driver.name);
            ui_->set_gpu_driver_status(imported_driver.name,
                                       imported_driver.message);
        } else {
            ui_->set_gpu_driver_status(
                settings_->get(settings_key::kGpuDriverName),
                imported_driver.message);
        }
        save_settings();
    }

    const RetroMediaResult retro_media =
        AndroidStorage::consume_retro_media_result();
    if (retro_media.ready) {
        if (retro_media.success) {
            ui_->set_retro_media_status(
                retro_media.email, retro_media.credits,
                retro_media.free_remaining, retro_media.is_admin, false,
                retro_media.message);
            if (retro_media.operation == "SYNC") load_retro_media_artwork();
            if (retro_media.operation == "CATALOGUE") {
                ui_->set_retro_media_catalogue(
                    AndroidStorage::retro_media_catalogue());
            }
            if (retro_media.operation == "DOWNLOAD") {
                scan_games_folder(
                    settings_->get(settings_key::kGamesFolderName),
                    settings_->get(settings_key::kGamesFolderPath));
            }
        } else if (retro_media.operation == "STATUS" ||
                   retro_media.message.find("session expired") !=
                       std::string::npos) {
            ui_->set_retro_media_status("", 0, 0, false, false,
                                        retro_media.message);
        } else {
            ui_->set_retro_media_error(retro_media.message);
        }
    }

    // Frames per second, smoothed, for the overlay. Measured across the whole
    // turn of the loop, so it reflects what the user actually sees.
    static Uint64 last_counter = SDL_GetPerformanceCounter();
    static double smoothed_fps = 0.0;
    const Uint64 now = SDL_GetPerformanceCounter();
    const double elapsed =
        static_cast<double>(now - last_counter) / SDL_GetPerformanceFrequency();
    last_counter = now;
    if (elapsed > 0.0) {
        const double instant = 1.0 / elapsed;
        smoothed_fps = smoothed_fps == 0.0 ? instant
                                           : smoothed_fps * 0.9 + instant * 0.1;
    }

    // Two different rates, and the difference matters: the emulator's frame
    // rate says whether the machine is keeping up, while the display's says
    // whether the app feels smooth. Showing the display rate alone would hide
    // a machine running at half speed behind a perfectly smooth window.
    const EmulatorStats emu = emulator_->stats();
    const UiIntent intent =
        ui_->build(*console_, emulating_, smoothed_fps, emu.emulated_fps,
                   emu.frame_ms, console_->audio().underruns(), touch_->enabled(),
                   touch_->editing());

    if (intent.bios_folder_chosen) {
        use_bios_folder(intent.folder_name, intent.folder_path);
    }
    if (intent.games_folder_chosen) {
        scan_games_folder(intent.folder_name, intent.folder_path);
    }
    if (intent.setup_finished) {
        settings_->set_bool(settings_key::kSetupComplete, true);
        save_settings();
    }
    if (intent.rescan_games) {
        scan_games_folder(settings_->get(settings_key::kGamesFolderName),
                          settings_->get(settings_key::kGamesFolderPath));
    }
    if (intent.import_gpu_driver) {
        AndroidStorage::pick_gpu_driver_package();
        ui_->set_gpu_driver_status(
            settings_->get(settings_key::kGpuDriverName),
            "Choose an ADPKG/ZIP driver package");
    }
    if (intent.use_system_gpu_driver) {
        settings_->remove(settings_key::kGpuDriverName);
        settings_->remove(settings_key::kGpuDriverDirectory);
        settings_->remove(settings_key::kGpuDriverLibrary);
        ui_->configure_video(ui_->renderer_backend(), ui_->widescreen(),
                             ui_->bezel(), ui_->crt_effect(), "");
        ui_->set_gpu_driver_status("", "System driver selected - restart to apply");
        save_settings();
    }
    if (intent.retro_media_login) {
        AndroidStorage::begin_retro_media_login(intent.retro_media_email,
                                                intent.retro_media_password);
    }
    if (intent.retro_media_logout) {
        AndroidStorage::begin_retro_media_logout();
    }
    if (intent.retro_media_artwork_changed) {
        load_retro_media_artwork();
    }
    if (intent.retro_media_sync) {
        std::vector<std::string> names;
        names.reserve(library_.games().size());
        for (const LibraryGame& game : library_.games()) {
            names.push_back(game.name);
        }
        AndroidStorage::begin_retro_media_sync(names,
                                               ui_->retro_media_type());
    }
    if (intent.retro_media_catalogue) {
        AndroidStorage::begin_retro_media_catalogue(intent.retro_media_search);
    }
    if (intent.retro_media_download) {
        AndroidStorage::begin_retro_media_download(
            intent.retro_media_slug,
            settings_->get(settings_key::kGamesFolderPath));
    }

    if (intent.bios_chosen && !intent.bios_path.empty()) {
        // A document URI is not a path and cannot be opened by name, so it goes
        // through the system for a descriptor instead. Everything downstream is
        // the same either way.
        if (open_bios(intent.bios_path, intent.bios_name)) {
            settings_->set(settings_key::kBiosPath, intent.bios_path);
            settings_->set(settings_key::kBiosName, intent.bios_name);
            settings_->save();
            SDL_Log("BIOS loaded");
        } else {
            SDL_Log("%s", console_->last_error().c_str());
        }
    }
    bool selected_disc_ready = false;
    if (intent.disc_chosen && !intent.disc_path.empty()) {
        if (open_disc(intent.disc_path, intent.disc_name)) {
            settings_->set(settings_key::kDiscPath, intent.disc_path);
            settings_->set(settings_key::kDiscName, intent.disc_name);
            if (!AndroidStorage::available()) {
                remember_game(intent.disc_name, intent.disc_path);
            }
            save_settings();
            selected_disc_ready = true;
            SDL_Log("Disc inserted (%u sectors)", console_->disc().sector_count());
        } else {
            if (intent.start_disc) ui_->show_launcher();
            SDL_Log("%s", console_->last_error().c_str());
        }
    }
    if (intent.eject) {
        console_->eject_disc();
    }
    if (intent.reset) {
        emulator_->request_reset();
        emulating_ = console_->bios_loaded();
        emulator_->set_paused(!emulating_);
    }
    if (intent.start_disc && selected_disc_ready) {
        emulator_->request_reset();
        emulating_ = console_->bios_loaded();
        emulator_->set_paused(!emulating_);
    }
    if (intent.toggle_pause) {
        emulating_ = !emulating_;
        emulator_->set_paused(!emulating_);
    }
    if (intent.region_changed) {
        // A region change resizes the framebuffer, so the emulator must not be
        // midway through a frame when it happens.
        emulator_->set_paused(true);
        console_->set_region(intent.set_region_pal ? Region::Pal : Region::Ntsc);
        mailbox_->resize(console_->framebuffer().width,
                         console_->framebuffer().height);
        emulator_->set_paused(!emulating_);
        save_settings();
    }
    if (intent.cpu_scale_changed) {
        console_->set_cpu_scale_percent(intent.cpu_scale_percent);
        save_settings();
    }
    if (intent.toggle_touch_controls) {
        touch_->set_visible(!touch_->enabled());
        if (!touch_->enabled()) touch_->set_editing(false);
        save_settings();
    }
    if (intent.toggle_layout_edit) {
        touch_->set_editing(!touch_->editing());
        if (!touch_->editing()) save_settings();
    }
    if (intent.reset_touch_layout) {
        int window_w = 0;
        int window_h = 0;
        SDL_GetRenderOutputSize(renderer_, &window_w, &window_h);
        touch_->reset_layout(window_w, window_h);
        save_settings();
    }
    if (intent.ui_settings_changed) {
        save_settings();
    }
    if (intent.quit) {
        running_ = false;
    }

    if (backgrounded_.load(std::memory_order_acquire)) return;

    SDL_SetRenderDrawColor(renderer_, 8, 8, 10, 255);
    SDL_RenderClear(renderer_);
    present();

    // Over the picture, under the menus: the controls belong to the game, not
    // to the launcher.
    {
        int window_w = 0;
        int window_h = 0;
        SDL_GetRenderOutputSize(renderer_, &window_w, &window_h);
        if (emulating_ || touch_->editing()) {
            touch_->draw(renderer_, window_w, window_h);
        }
    }

    ui_->render(renderer_);
    if (backgrounded_.load(std::memory_order_acquire)) return;
    SDL_RenderPresent(renderer_);
}

int App::run() {
    while (running_) {
        tick();
    }
    return 0;
}

}  // namespace retro3do
