#include "ui.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "core/console.h"
#include "platform/storage.h"
#include "imgui.h"

namespace retro3do {

Ui::Ui() = default;

Ui::~Ui() {
    shutdown();
}

bool Ui::init(SDL_Window* window, SDL_Renderer* renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // No imgui.ini: on mobile there is nowhere sensible to write it, and a
    // remembered window layout is not wanted on a fixed-size screen anyway.
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    // Scale the whole UI to the screen.
    //
    // SDL_GetWindowDisplayScale reports 1.0 on many Android devices, so it
    // cannot be relied on alone: a 1080p handheld would get ImGui's 13-pixel
    // default font, which is genuinely unreadable at arm's length. The window's
    // own size is the honest signal, with the reported display scale taken as a
    // floor for desktops with a HiDPI screen and a small window.
    int window_w = 0;
    int window_h = 0;
    SDL_GetWindowSizeInPixels(window, &window_w, &window_h);

    const float from_size =
        static_cast<float>(window_h > 0 ? window_h : 720) / 480.0f;
    const float reported = SDL_GetWindowDisplayScale(window);
    scale_ = from_size > reported ? from_size : reported;
    if (scale_ < 1.0f) scale_ = 1.0f;
    if (scale_ > 4.0f) scale_ = 4.0f;

    ImGui::GetStyle().ScaleAllSizes(scale_);
    io.FontGlobalScale = scale_;

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
        return false;
    }
    if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
        return false;
    }

    initialised_ = true;
    return true;
}

void Ui::shutdown() {
    if (!initialised_) {
        return;
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    initialised_ = false;
}

void Ui::process_event(const SDL_Event& event) {
    if (!initialised_) return;
    ImGui_ImplSDL3_ProcessEvent(&event);
}

bool Ui::wants_mouse() const {
    return initialised_ && ImGui::GetIO().WantCaptureMouse;
}

bool Ui::wants_keyboard() const {
    return initialised_ && ImGui::GetIO().WantCaptureKeyboard;
}

UiIntent Ui::build(Console& console, bool emulating, double display_fps,
                   double emulated_fps, double frame_ms, u64 underruns,
                   bool touch_visible, bool touch_editing) {
    UiIntent intent;
    if (!initialised_) {
        return intent;
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (show_launcher_ || !emulating) {
        draw_launcher(console, intent);
    } else {
        draw_overlay(console, display_fps, emulated_fps, frame_ms, underruns,
                     touch_visible, touch_editing, intent);
    }

    ImGui::Render();
    return intent;
}

void Ui::draw_launcher(Console& console, UiIntent& intent) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::Begin("Retro-3DO", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextUnformatted("Retro-3DO");
    ImGui::TextDisabled("A 3DO Interactive Multiplayer emulator");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("System ROM");
    ImGui::SetNextItemWidth(-(230.0f * scale_));
    ImGui::InputTextWithHint("##bios", "path to the 3DO BIOS image",
                             bios_path_buffer_, sizeof(bios_path_buffer_));
    ImGui::SameLine();
    if (ImGui::Button("Browse##bios", ImVec2(110.0f * scale_, 0.0f))) {
        browsing_ = Browsing::Bios;
        browser_.open("Choose a BIOS image", {".rom", ".bin"});
    }
    ImGui::SameLine();
    if (ImGui::Button("Load", ImVec2(80.0f * scale_, 0.0f))) {
        intent.bios_chosen = true;
        intent.bios_path = bios_path_buffer_;
    }

    if (console.bios_loaded()) {
        ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.60f, 1.0f), "BIOS loaded.");
    } else if (!console.last_error().empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "%s",
                           console.last_error().c_str());
    } else {
        ImGui::TextDisabled("No BIOS loaded yet. The machine needs one to boot.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Disc");
    ImGui::SetNextItemWidth(-(230.0f * scale_));
    ImGui::InputTextWithHint("##disc", "path to a .iso, .bin or .cue",
                             disc_path_buffer_, sizeof(disc_path_buffer_));
    ImGui::SameLine();
    if (ImGui::Button("Browse##disc", ImVec2(110.0f * scale_, 0.0f))) {
        browsing_ = Browsing::Disc;
        browser_.open("Choose a disc image", {".iso", ".bin", ".cue", ".img"});
    }
    ImGui::SameLine();
    if (ImGui::Button("Insert", ImVec2(80.0f * scale_, 0.0f))) {
        intent.disc_chosen = true;
        intent.disc_path = disc_path_buffer_;
    }

    if (console.disc_loaded()) {
        const Disc& disc = console.disc();
        const char* layout_name = "cooked 2048";
        switch (disc.layout()) {
            case SectorLayout::Raw2352Mode1: layout_name = "raw 2352, mode 1"; break;
            case SectorLayout::Raw2352Mode2: layout_name = "raw 2352, mode 2"; break;
            case SectorLayout::Raw2336Mode2: layout_name = "raw 2336, mode 2"; break;
            case SectorLayout::Cooked2048:   break;
        }
        ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.60f, 1.0f),
                           "Disc in: %u sectors, %s, %zu track%s.",
                           disc.sector_count(), layout_name, disc.tracks().size(),
                           disc.tracks().size() == 1 ? "" : "s");

        // The track list is worth showing: it is the quickest way to tell a
        // good dump from one whose cue sheet does not match its image.
        if (ImGui::TreeNode("Tracks")) {
            for (const Track& track : disc.tracks()) {
                ImGui::Text("%2u  %-5s  start %6u  length %6u", track.number,
                            track.is_audio ? "audio" : "data", track.start_lba,
                            track.length_sectors);
            }
            ImGui::TreePop();
        }
        if (ImGui::SmallButton("Eject")) {
            intent.eject = true;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginDisabled(!console.bios_loaded());
    if (ImGui::Button("Start", ImVec2(150.0f * scale_, 0.0f))) {
        show_launcher_ = false;
        intent.reset = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Test pattern", ImVec2(150.0f * scale_, 0.0f))) {
        // Draws without a BIOS. It exercises the whole display path - bus into
        // VRAM, display list, VDLP, texture upload, letterboxing - so on a new
        // device it separates "the video path is broken" from "the emulator is
        // not running", which otherwise look identical.
        intent.test_pattern = true;
        show_launcher_ = false;
    }

    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(150.0f * scale_, 0.0f))) {
        intent.quit = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(
        "Under construction. The CPU, interrupts, graphics and display path all "
        "work - the test pattern proves them. What is missing is the link "
        "between the disc and the CPU, so a real disc will not boot yet.");
    ImGui::PopTextWrapPos();

    ImGui::End();

    // Drawn after the launcher so it floats above it, and routed to whichever
    // field asked for it.
    std::string picked;
    std::string picked_name;
    if (browser_.draw(&picked, &picked_name)) {
        if (browsing_ == Browsing::Bios) {
            std::snprintf(bios_path_buffer_, sizeof(bios_path_buffer_), "%s",
                          picked_name.empty() ? picked.c_str() : picked_name.c_str());
            intent.bios_chosen = true;
            intent.bios_path = picked;
            intent.bios_name = picked_name;
        } else if (browsing_ == Browsing::Disc) {
            std::snprintf(disc_path_buffer_, sizeof(disc_path_buffer_), "%s",
                          picked_name.empty() ? picked.c_str() : picked_name.c_str());
            intent.disc_chosen = true;
            intent.disc_path = picked;
            intent.disc_name = picked_name;
        }
        browsing_ = Browsing::None;
    }
}

void Ui::draw_overlay(Console& console, double display_fps,
                      double emulated_fps, double frame_ms, u64 underruns,
                      bool touch_visible, bool touch_editing, UiIntent& intent) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + 12.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);

    ImGui::Begin("##overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

    // The target rate, so "48" reads as "behind" rather than just as a number.
    const double target = console.region() == Region::Pal ? 50.0 : 60.0;
    const bool keeping_up = emulated_fps >= target - 2.0;

    ImGui::TextColored(keeping_up ? ImVec4(0.45f, 0.80f, 0.60f, 1.0f)
                                  : ImVec4(0.95f, 0.65f, 0.35f, 1.0f),
                       "machine %.0f/%.0f fps", emulated_fps, target);
    ImGui::Text("display %.0f fps", display_fps);
    ImGui::Text("frame   %.2f ms", frame_ms);
    if (underruns > 0) {
        // The first symptom of falling behind, and it shows up before anything
        // is visible on screen.
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "audio gaps %llu",
                           static_cast<unsigned long long>(underruns));
    }

    ImGui::Separator();
    ImGui::Text("PC %08X", console.cpu().pc());

    ImGui::Separator();
    if (ImGui::Button("Menu")) {
        show_launcher_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        intent.reset = true;
    }

    ImGui::Separator();
    if (ImGui::Button(touch_visible ? "Hide pad" : "Show pad")) {
        intent.toggle_touch_controls = true;
    }
    if (touch_visible) {
        ImGui::SameLine();
        // Layout mode makes dragging move the controls instead of pressing
        // them, which is the only way to fix a layout that does not suit a
        // particular pair of hands or a particular phone.
        if (ImGui::Button(touch_editing ? "Done" : "Move pad")) {
            intent.toggle_layout_edit = true;
        }
        if (touch_editing) {
            ImGui::SameLine();
            if (ImGui::Button("Reset pad")) {
                intent.reset_touch_layout = true;
            }
            ImGui::TextDisabled("Drag the controls where you want them.");
        }
    }

    ImGui::End();
}

void Ui::render(SDL_Renderer* renderer) {
    if (!initialised_) return;
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

}  // namespace retro3do
