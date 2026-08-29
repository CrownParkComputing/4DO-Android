#include "app.h"

#include <SDL3/SDL.h>

#include <cstring>

#include "core/console.h"
#include "core/frame_mailbox.h"
#include "core/pad.h"
#include "platform/android_storage.h"
#include "platform/emulator_thread.h"
#include "ui/ui.h"

namespace retro3do {
namespace {

constexpr int kDefaultWindowWidth  = 1280;
constexpr int kDefaultWindowHeight = 960;

// Fill VRAM with a colour ramp and build a one-entry display list over it, so
// the machine draws something without a BIOS. This goes through the real bus
// and the real VDLP: if it appears, the whole video path works.
void write_test_pattern(Console& console) {
    Bus& bus = console.bus();
    const Frame frame = console.framebuffer();

    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const unsigned r = static_cast<unsigned>(x * 31 / (frame.width - 1));
            const unsigned g = static_cast<unsigned>(y * 31 / (frame.height - 1));
            const unsigned b = 31u - r;
            const u16 pixel = static_cast<u16>((r << 10) | (g << 5) | b);

            const u32 offset = static_cast<u32>(y * frame.width + x) * 2u;
            bus.write16(kVramBase + offset, pixel);
        }
    }

    // A display list in DRAM pointing at the start of VRAM.
    const u32 list = 0x1000u;
    bus.write32(list + 0, static_cast<u32>(frame.height));
    bus.write32(list + 4, kVramBase);
    bus.write32(list + 8, kVramBase);
    bus.write32(list + 12, 0);
    console.vdlp().set_list_address(list);
}

}  // namespace

App::App() = default;

App::~App() {
    // Stop emulating before anything it touches goes away.
    if (emulator_) emulator_->stop();
    emulator_.reset();
    ui_.reset();
    stop_audio();
    if (gamepad_) SDL_CloseGamepad(gamepad_);
    if (frame_texture_) SDL_DestroyTexture(frame_texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool App::init() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        last_error_ = std::string("SDL could not start: ") + SDL_GetError();
        return false;
    }

    // On a phone or tablet there is no window to size — SDL gives us the
    // display. Asking for a resizable window is harmless there and correct on
    // desktop.
    if (!SDL_CreateWindowAndRenderer("Retro-3DO", kDefaultWindowWidth,
                                     kDefaultWindowHeight,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &window_, &renderer_)) {
        last_error_ = std::string("Could not create a window: ") + SDL_GetError();
        return false;
    }

    // Present without waiting on vblank. The emulator paces itself from its own
    // frame budget; letting the present block would put the display's refresh
    // rate in charge of emulation speed, which is the mistake that made the
    // previous version stutter on handhelds.
    SDL_SetRenderVSync(renderer_, 0);

    console_ = std::make_unique<Console>();
    mailbox_ = std::make_unique<FrameMailbox>();
    emulator_ = std::make_unique<EmulatorThread>(*console_, *mailbox_);

    ui_ = std::make_unique<Ui>();
    if (!ui_->init(window_, renderer_)) {
        last_error_ = "Could not start the user interface";
        return false;
    }

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
    emulator_->set_paused(true);
    emulator_->start();

    running_ = true;
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
        // Nearest keeps the pixels honest; a smoothing filter is a user choice,
        // not a default.
        SDL_SetTextureScaleMode(frame_texture_, SDL_SCALEMODE_NEAREST);
        texture_width_  = width;
        texture_height_ = height;
    }
}

void App::handle_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ui_->process_event(event);

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
                if (gamepad_ != nullptr &&
                    SDL_GetGamepadID(gamepad_) == event.gdevice.which) {
                    SDL_CloseGamepad(gamepad_);
                    gamepad_ = nullptr;
                }
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                PadState& pads = console_->pads();
                switch (event.gbutton.button) {
                    case SDL_GAMEPAD_BUTTON_DPAD_UP:    pads.press(0, PadButton::Up, down); break;
                    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  pads.press(0, PadButton::Down, down); break;
                    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  pads.press(0, PadButton::Left, down); break;
                    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: pads.press(0, PadButton::Right, down); break;
                    case SDL_GAMEPAD_BUTTON_SOUTH:      pads.press(0, PadButton::A, down); break;
                    case SDL_GAMEPAD_BUTTON_EAST:       pads.press(0, PadButton::B, down); break;
                    case SDL_GAMEPAD_BUTTON_WEST:       pads.press(0, PadButton::C, down); break;
                    case SDL_GAMEPAD_BUTTON_START:      pads.press(0, PadButton::Play, down); break;
                    case SDL_GAMEPAD_BUTTON_BACK:       pads.press(0, PadButton::Stop, down); break;
                    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  pads.press(0, PadButton::LeftShift, down); break;
                    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: pads.press(0, PadButton::RightShift, down); break;
                    default: break;
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

    // Letterbox to the machine's 4:3 output rather than stretching to the
    // window, which on a phone in landscape would otherwise distort everything.
    int window_w = 0;
    int window_h = 0;
    SDL_GetRenderOutputSize(renderer_, &window_w, &window_h);

    const float target_aspect = 4.0f / 3.0f;
    (void)height;
    float draw_w = static_cast<float>(window_w);
    float draw_h = draw_w / target_aspect;
    if (draw_h > static_cast<float>(window_h)) {
        draw_h = static_cast<float>(window_h);
        draw_w = draw_h * target_aspect;
    }

    SDL_FRect destination;
    destination.x = (static_cast<float>(window_w) - draw_w) * 0.5f;
    destination.y = (static_cast<float>(window_h) - draw_h) * 0.5f;
    destination.w = draw_w;
    destination.h = draw_h;

    SDL_RenderTexture(renderer_, frame_texture_, nullptr, &destination);
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
    SDL_ResumeAudioStreamDevice(audio_stream_);
    return true;
}

void App::stop_audio() {
    if (audio_stream_ != nullptr) {
        SDL_DestroyAudioStream(audio_stream_);
        audio_stream_ = nullptr;
    }
}

void App::feed_audio() {
    if (audio_stream_ == nullptr) {
        return;
    }

    // Keep roughly two frames queued. Less and any hitch is audible; more and
    // input starts to feel detached from the sound.
    constexpr int kTargetQueuedBytes =
        static_cast<int>(kAudioSampleRate / 30) * static_cast<int>(sizeof(StereoSample));

    const int queued = SDL_GetAudioStreamQueued(audio_stream_);
    if (queued >= kTargetQueuedBytes) {
        return;
    }

    StereoSample chunk[1024];
    int wanted = (kTargetQueuedBytes - queued) / static_cast<int>(sizeof(StereoSample));
    while (wanted > 0) {
        const u32 count = static_cast<u32>(wanted > 1024 ? 1024 : wanted);
        console_->audio().pull(chunk, count);
        SDL_PutAudioStreamData(audio_stream_, chunk,
                               static_cast<int>(count * sizeof(StereoSample)));
        wanted -= static_cast<int>(count);
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

void App::open_gamepad(u32 which) {
    if (gamepad_ != nullptr) {
        return;  // one pad for now; the 3DO chains up to eight
    }
    gamepad_ = SDL_OpenGamepad(which);
    if (gamepad_ != nullptr) {
        SDL_Log("Gamepad connected: %s", SDL_GetGamepadName(gamepad_));
    }
}

void App::tick() {
    handle_events();

    feed_audio();

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
                   emu.frame_ms, console_->audio().underruns());

    if (intent.bios_chosen && !intent.bios_path.empty()) {
        // A document URI is not a path and cannot be opened by name, so it goes
        // through the system for a descriptor instead. Everything downstream is
        // the same either way.
        const bool ok =
            AndroidStorage::available()
                ? console_->load_bios_fd(
                      AndroidStorage::open_document(intent.bios_path),
                      intent.bios_name.empty() ? intent.bios_path : intent.bios_name)
                : console_->load_bios(intent.bios_path);
        SDL_Log("%s", ok ? "BIOS loaded" : console_->last_error().c_str());
    }
    if (intent.disc_chosen && !intent.disc_path.empty()) {
        const bool ok =
            AndroidStorage::available()
                ? console_->load_disc_fd(
                      AndroidStorage::open_document(intent.disc_path),
                      intent.disc_name.empty() ? intent.disc_path : intent.disc_name)
                : console_->load_disc(intent.disc_path);
        if (ok) {
            SDL_Log("Disc inserted (%u sectors)", console_->disc().sector_count());
        } else {
            SDL_Log("%s", console_->last_error().c_str());
        }
    }
    if (intent.eject) {
        console_->eject_disc();
    }
    if (intent.test_pattern) {
        // Drawn on this thread, with the emulator paused, so there is no race
        // for the console's memory.
        emulator_->set_paused(true);
        console_->reset();
        write_test_pattern(*console_);
        console_->run_frame();
        std::memcpy(mailbox_->writable(), console_->framebuffer().pixels,
                    static_cast<size_t>(mailbox_->width()) * mailbox_->height() *
                        sizeof(u32));
        mailbox_->publish();
        emulating_ = false;
    }
    if (intent.reset) {
        emulator_->request_reset();
        emulating_ = console_->bios_loaded();
        emulator_->set_paused(!emulating_);
    }
    if (intent.toggle_pause) {
        emulating_ = !emulating_;
        emulator_->set_paused(!emulating_);
    }
    if (intent.quit) {
        running_ = false;
    }

    SDL_SetRenderDrawColor(renderer_, 8, 8, 10, 255);
    SDL_RenderClear(renderer_);
    present();
    ui_->render(renderer_);
    SDL_RenderPresent(renderer_);
}

int App::run() {
    while (running_) {
        tick();
    }
    return 0;
}

}  // namespace retro3do
