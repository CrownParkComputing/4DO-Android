#include "app.h"

#include <SDL3/SDL.h>

#include <cstring>

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

// One NVRAM, shared by every title, exactly as a console has one. Keeping a
// separate image per disc would be tidier to reason about, but it is not what
// the hardware does and it would break the titles that read each other's saves.
constexpr const char* kNvramFile = "nvram.bin";

}  // namespace

App::App() = default;

App::~App() {
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

    // On a phone or tablet there is no window to size — SDL gives us the
    // display. Asking for a resizable window is harmless there and correct on
    // desktop.
    // Fullscreen on a handheld, a window on a desktop. Without this the system
    // status bar sits on top of the app: not merely untidy, it covers the top
    // row of controls and steals touches meant for them.
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__ANDROID__) || defined(__APPLE__)
    flags |= SDL_WINDOW_FULLSCREEN;
#else
    flags |= SDL_WINDOW_RESIZABLE;
#endif

    if (!SDL_CreateWindowAndRenderer("Retro-3DO", kDefaultWindowWidth,
                                     kDefaultWindowHeight, flags,
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
    settings_ = std::make_unique<Settings>();
    touch_ = std::make_unique<TouchPad>();
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

    emulator_->set_paused(!start_on_launch_);
    emulating_ = start_on_launch_;
    if (start_on_launch_) {
        ui_->hide_launcher();
    }
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

        // Touch controls get first refusal, but only when the UI is not using
        // the pointer: otherwise a tap on a menu would also press a button
        // hiding underneath it.
        if (!ui_->wants_mouse()) {
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

    console_->set_region(settings_->get_int(settings_key::kRegion, 0) == 1
                             ? Region::Pal
                             : Region::Ntsc);
    touch_->load_layout(*settings_);

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
            // Start straight away. Having to press Start on every launch when
            // the machine already has everything it needs is a chore, and it
            // means the app looks inert on opening when it is not.
            start_on_launch_ = true;
        } else {
            SDL_Log("Remembered BIOS is no longer reachable; forgetting it");
            settings_->remove(settings_key::kBiosPath);
            settings_->remove(settings_key::kBiosName);
        }
    }

    const std::string disc = launch_disc_.empty()
                                 ? settings_->get(settings_key::kDiscPath)
                                 : launch_disc_;
    ui_->set_remembered_disc(settings_->get(settings_key::kDiscName), disc);
    if (!disc.empty()) {
        if (!open_disc(disc, launch_disc_.empty()
                                 ? settings_->get(settings_key::kDiscName)
                                 : Storage::base_name(launch_disc_))) {
            settings_->remove(settings_key::kDiscPath);
            settings_->remove(settings_key::kDiscName);
        }
    }
}

void App::save_settings() {
    settings_->set_int(settings_key::kRegion,
                       console_->region() == Region::Pal ? 1 : 0);
    touch_->save_layout(*settings_);
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

    feed_audio();
    save_nvram();

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
                   emu.frame_ms, console_->audio().underruns(), touch_->visible(),
                   touch_->editing());

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
    if (intent.disc_chosen && !intent.disc_path.empty()) {
        if (open_disc(intent.disc_path, intent.disc_name)) {
            settings_->set(settings_key::kDiscPath, intent.disc_path);
            settings_->set(settings_key::kDiscName, intent.disc_name);
            settings_->save();
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
    if (intent.toggle_touch_controls) {
        touch_->set_visible(!touch_->visible());
        if (!touch_->visible()) touch_->set_editing(false);
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
    if (intent.quit) {
        running_ = false;
    }

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
    SDL_RenderPresent(renderer_);
}

int App::run() {
    while (running_) {
        tick();
    }
    return 0;
}

}  // namespace retro3do
