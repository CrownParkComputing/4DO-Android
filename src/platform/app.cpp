#include "app.h"

#include <SDL3/SDL.h>

#include "core/console.h"
#include "ui/ui.h"

namespace retro3do {
namespace {

constexpr int kDefaultWindowWidth  = 1280;
constexpr int kDefaultWindowHeight = 960;

}  // namespace

App::App() = default;

App::~App() {
    ui_.reset();
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

    ui_ = std::make_unique<Ui>();
    if (!ui_->init(window_, renderer_)) {
        last_error_ = "Could not start the user interface";
        return false;
    }

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
            default:
                break;
        }
    }
}

void App::present() {
    const Frame frame = console_->framebuffer();
    if (frame.pixels == nullptr || frame.width <= 0 || frame.height <= 0) {
        return;
    }

    ensure_frame_texture(frame.width, frame.height);
    if (frame_texture_ == nullptr) {
        return;
    }

    SDL_UpdateTexture(frame_texture_, nullptr, frame.pixels,
                      frame.width * static_cast<int>(sizeof(u32)));

    // Letterbox to the machine's 4:3 output rather than stretching to the
    // window, which on a phone in landscape would otherwise distort everything.
    int window_w = 0;
    int window_h = 0;
    SDL_GetRenderOutputSize(renderer_, &window_w, &window_h);

    const float target_aspect = 4.0f / 3.0f;
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

void App::tick() {
    handle_events();

    if (emulating_ && console_->bios_loaded()) {
        console_->run_frame();
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

    const UiIntent intent = ui_->build(*console_, emulating_, smoothed_fps);

    if (intent.bios_chosen && !intent.bios_path.empty()) {
        if (console_->load_bios(intent.bios_path)) {
            SDL_Log("BIOS loaded from %s", intent.bios_path.c_str());
        } else {
            SDL_Log("%s", console_->last_error().c_str());
        }
    }
    if (intent.reset) {
        console_->reset();
        emulating_ = console_->bios_loaded();
    }
    if (intent.toggle_pause) {
        emulating_ = !emulating_;
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
