#include "ui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "core/console.h"
#include "imgui.h"
#include "platform/android_storage.h"
#include "platform/platform.h"

namespace retro3do {
namespace {

const ImVec4 kCyan(0.18f, 0.84f, 0.88f, 1.0f);
const ImVec4 kGreen(0.36f, 0.86f, 0.58f, 1.0f);
const ImVec4 kAmber(1.00f, 0.68f, 0.28f, 1.0f);
const ImVec4 kRed(1.00f, 0.37f, 0.38f, 1.0f);
const ImVec4 kViolet(0.70f, 0.45f, 1.00f, 1.0f);
const ImVec4 kPink(1.00f, 0.38f, 0.68f, 1.0f);
const ImVec4 kBlue(0.30f, 0.58f, 1.00f, 1.0f);
const ImVec4 kMuted(0.55f, 0.60f, 0.68f, 1.0f);

ImU32 colour(const ImVec4& value) {
    return ImGui::ColorConvertFloat4ToU32(value);
}

void page_heading(const char* title, const char* subtitle) {
    // Settings and System must remain complete, single-screen pages on a
    // landscape handheld. Keep their identity in one compact row instead of
    // spending nearly a quarter of the viewport on a display-sized heading.
    ImGui::TextColored(kCyan, "%s", title);
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%s", subtitle);
    ImGui::Separator();
    ImGui::Spacing();
}

void section_heading(const char* title, const char* detail) {
    ImGui::TextColored(kCyan, "%s", title);
    if (detail != nullptr && detail[0] != '\0') {
        ImGui::TextColored(kMuted, "%s", detail);
    }
    ImGui::Spacing();
}

bool wide_button(const char* label, float height) {
    return ImGui::Button(label, ImVec2(-1.0f, height));
}

char card_initial(const std::string& name) {
    for (unsigned char c : name) {
        if (std::isalnum(c)) return static_cast<char>(std::toupper(c));
    }
    return '?';
}

ImVec4 card_accent(const std::string& name) {
    static const ImVec4 palette[] = {kCyan, kViolet, kPink, kAmber, kGreen,
                                     kBlue};
    unsigned hash = 2166136261u;
    for (unsigned char c : name) hash = (hash ^ c) * 16777619u;
    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

ImVec4 tinted(const ImVec4& colour_value, float strength, float alpha = 1.0f) {
    return ImVec4(0.035f + colour_value.x * strength,
                  0.045f + colour_value.y * strength,
                  0.070f + colour_value.z * strength, alpha);
}

void draw_card_title(ImDrawList* draw, const std::string& title,
                     const ImVec2& position, float max_width, ImU32 colour_value,
                     float font_size) {
    // Keep titles to one draw command. Thousands of tiny per-glyph geometry
    // submissions made the SDL renderer drop later commands on dense library
    // screens, which showed up as alternating cards with no title.
    ImFont* font = ImGui::GetFont();
    std::string shown = title;
    const char* suffix = "...";
    const float width_limit = std::max(1.0f, max_width);
    if (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, shown.c_str()).x >
        width_limit) {
        while (!shown.empty()) {
            shown.pop_back();
            const std::string candidate = shown + suffix;
            if (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f,
                                    candidate.c_str()).x <= width_limit) {
                shown = candidate;
                break;
            }
        }
    }
    const float shown_width =
        font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, shown.c_str()).x;
    draw->AddText(font, font_size,
                  ImVec2(position.x + std::max(0.0f,
                                               (width_limit - shown_width) * 0.5f),
                         position.y),
                  colour_value, shown.c_str());
}

}  // namespace

Ui::Ui() = default;

Ui::~Ui() {
    shutdown();
}

bool Ui::init(SDL_Window* window, SDL_Renderer* renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
#if RETRO3DO_MOBILE
    // iPad and iPhone want this for the same reason Android does: it widens
    // ImGui's hit areas to finger size. Not on macOS, which has a pointer.
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
#endif
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildRounding = 8.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.WindowPadding = ImVec2(18.0f, 18.0f);
    style.FramePadding = ImVec2(12.0f, 8.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.045f, 0.070f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.070f, 0.105f, 1.0f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.14f, 0.19f, 0.26f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.075f, 0.095f, 0.140f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.11f, 0.16f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.09f, 0.13f, 0.19f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.12f, 0.35f, 0.40f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.55f, 0.60f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.28f, 0.34f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.12f, 0.38f, 0.43f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = kCyan;
    style.Colors[ImGuiCol_SliderGrab] = kCyan;
    style.Colors[ImGuiCol_Text] = ImVec4(0.91f, 0.94f, 0.97f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = kMuted;

    int window_w = 0;
    int window_h = 0;
    SDL_GetWindowSizeInPixels(window, &window_w, &window_h);
    const float from_size =
        static_cast<float>(window_h > 0 ? window_h : 720) / 480.0f;
    const float reported = SDL_GetWindowDisplayScale(window);
    scale_ = std::max(from_size, reported);
    scale_ = std::clamp(scale_, 1.0f, 4.0f);
    style.ScaleAllSizes(scale_);
    io.FontGlobalScale = scale_;

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) return false;
    if (!ImGui_ImplSDLRenderer3_Init(renderer)) return false;

    renderer_ = renderer;
    load_splash_texture();

    splash_started_ms_ = SDL_GetTicks();
    last_pointer_activity_ms_ = splash_started_ms_;
    initialised_ = true;
    return true;
}

void Ui::wake_menu_button() {
    last_pointer_activity_ms_ = SDL_GetTicks();
}

void Ui::hide_launcher() {
    show_launcher_ = false;
    session_available_ = true;
    wake_menu_button();
}

void Ui::set_remembered_bios(const std::string& name,
                             const std::string& target) {
    std::snprintf(bios_path_buffer_, sizeof(bios_path_buffer_), "%s",
                  name.empty() ? target.c_str() : name.c_str());
    bios_target_ = target;
}

void Ui::set_remembered_disc(const std::string& name,
                             const std::string& target) {
    std::snprintf(disc_path_buffer_, sizeof(disc_path_buffer_), "%s",
                  name.empty() ? target.c_str() : name.c_str());
    disc_target_ = target;
}

void Ui::set_game_library(const std::vector<LibraryGame>& games) {
    library_.replace(games);
}

void Ui::configure_video(int renderer_backend, bool widescreen, bool bezel,
                         bool crt_effect, const std::string& gpu_driver_name) {
    renderer_backend_ = renderer_backend == 1 ? 1 : 0;
    widescreen_ = widescreen;
    bezel_ = bezel;
    crt_effect_ = crt_effect;
    gpu_driver_name_ = gpu_driver_name;
}

void Ui::set_renderer_status(const std::string& actual,
                             const std::string& startup_message) {
    actual_renderer_ = actual;
    renderer_startup_message_ = startup_message;
}

void Ui::set_gpu_driver_status(const std::string& name,
                               const std::string& message) {
    gpu_driver_name_ = name;
    gpu_driver_message_ = message;
}

void Ui::configure_retro_media(const std::string& media_type) {
    static constexpr const char* kTypes[] = {
        "box2d", "images", "thumbnails", "titles"
    };
    retro_media_type_ = "box2d";
    for (const char* type : kTypes) {
        if (media_type == type) retro_media_type_ = type;
    }
}

void Ui::set_retro_media_status(const std::string& email, int credits,
                                int free_remaining, bool is_admin, bool busy,
                                const std::string& message) {
    retro_media_email_ = email;
    retro_media_credits_ = credits;
    retro_media_free_remaining_ = free_remaining;
    retro_media_admin_ = is_admin;
    if (!is_admin && page_ == Page::Downloads) page_ = Page::Artwork;
    retro_media_busy_ = busy;
    retro_media_message_ = message;
    if (!email.empty()) {
        std::snprintf(retro_media_email_buffer_,
                      sizeof(retro_media_email_buffer_), "%s", email.c_str());
    }
    if (!busy) {
        // The password is only needed to make the server session. Do not keep
        // it in the UI buffer after that attempt succeeds or fails.
        std::memset(retro_media_password_buffer_, 0,
                    sizeof(retro_media_password_buffer_));
    }
}

void Ui::set_retro_media_catalogue(
    const std::vector<RetroMediaGame>& games) {
    retro_media_catalogue_.clear();
    retro_media_catalogue_.reserve(games.size());
    for (const RetroMediaGame& game : games) {
        retro_media_catalogue_.push_back(
            {game.slug, game.name, game.rom_files, game.total_bytes});
    }
}

void Ui::set_retro_media_error(const std::string& message) {
    retro_media_busy_ = false;
    retro_media_message_ = message;
    std::memset(retro_media_password_buffer_, 0,
                sizeof(retro_media_password_buffer_));
}

void Ui::set_retro_media_email_hint(const std::string& email) {
    if (email.empty() || retro_media_email_buffer_[0] != '\0') return;
    std::snprintf(retro_media_email_buffer_, sizeof(retro_media_email_buffer_),
                  "%s", email.c_str());
}

void Ui::clear_artwork_textures() {
    release_artwork_textures();
    artwork_.clear();
}

void Ui::load_splash_texture() {
    if (renderer_ == nullptr || splash_texture_ != nullptr) return;
    SDL_Surface* surface = SDL_LoadBMP("retro3do-splash.bmp");
    if (surface == nullptr) {
        surface = SDL_LoadBMP("assets/branding/retro3do-splash.bmp");
    }
    if (surface == nullptr) return;
    splash_texture_ = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_DestroySurface(surface);
    if (splash_texture_ != nullptr) {
        SDL_SetTextureScaleMode(splash_texture_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(splash_texture_, SDL_BLENDMODE_BLEND);
    }
}

void Ui::release_artwork_textures() {
    for (auto& [name, item] : artwork_) {
        (void)name;
        if (item.texture != nullptr) SDL_DestroyTexture(item.texture);
        item.texture = nullptr;
        item.attempted = false;
    }
}

void Ui::set_retro_media_artwork(
    const std::vector<RetroMediaArtwork>& artwork) {
    clear_artwork_textures();
    for (const RetroMediaArtwork& item : artwork) {
        if (item.key.empty() || item.path.empty() || item.width <= 0 ||
            item.height <= 0) {
            continue;
        }
        artwork_.emplace(item.key, ArtworkTexture{item.path, item.width,
                                                  item.height, nullptr, false});
    }
}

SDL_Texture* Ui::artwork_texture(const std::string& game_name) {
    const auto found = artwork_.find(game_name);
    if (found == artwork_.end()) return nullptr;
    ArtworkTexture& item = found->second;
    if (item.texture != nullptr || item.attempted || renderer_ == nullptr) {
        return item.texture;
    }
    item.attempted = true;

    size_t size = 0;
    void* file = SDL_LoadFile(item.path.c_str(), &size);
    if (file == nullptr || size < 12) {
        if (file != nullptr) SDL_free(file);
        return nullptr;
    }
    const u8* bytes = static_cast<const u8*>(file);
    const auto big_endian_u32 = [](const u8* value) {
        return (static_cast<u32>(value[0]) << 24u) |
               (static_cast<u32>(value[1]) << 16u) |
               (static_cast<u32>(value[2]) << 8u) |
               static_cast<u32>(value[3]);
    };
    const u32 width = big_endian_u32(bytes + 4);
    const u32 height = big_endian_u32(bytes + 8);
    const size_t pixel_bytes = static_cast<size_t>(width) * height * 4u;
    if (std::memcmp(bytes, "R3AR", 4) != 0 || width == 0 || height == 0 ||
        width > 2048 || height > 2048 || pixel_bytes != size - 12u) {
        SDL_free(file);
        return nullptr;
    }

    item.texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STATIC,
                                     static_cast<int>(width),
                                     static_cast<int>(height));
    if (item.texture != nullptr) {
        SDL_UpdateTexture(item.texture, nullptr, bytes + 12,
                          static_cast<int>(width * 4u));
        SDL_SetTextureScaleMode(item.texture, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(item.texture, SDL_BLENDMODE_BLEND);
        item.width = static_cast<int>(width);
        item.height = static_cast<int>(height);
    }
    SDL_free(file);
    return item.texture;
}

void Ui::configure_setup(bool complete, const std::string& bios_folder_name,
                         const std::string& bios_folder_target,
                         const std::string& games_folder_name,
                         const std::string& games_folder_target) {
    setup_complete_ = complete;
    bios_folder_name_ = bios_folder_name;
    bios_folder_target_ = bios_folder_target;
    games_folder_name_ = games_folder_name;
    games_folder_target_ = games_folder_target;
    if (!complete) wizard_step_ = WizardStep::Welcome;
}

void Ui::shutdown() {
    if (!initialised_) return;
    if (splash_texture_ != nullptr) {
        SDL_DestroyTexture(splash_texture_);
        splash_texture_ = nullptr;
    }
    clear_artwork_textures();
    if (renderer_ != nullptr) ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    renderer_ = nullptr;
    initialised_ = false;
}

void Ui::suspend_renderer() {
    if (!initialised_ || renderer_ == nullptr) return;
    if (splash_texture_ != nullptr) {
        SDL_DestroyTexture(splash_texture_);
        splash_texture_ = nullptr;
    }
    release_artwork_textures();
    ImGui_ImplSDLRenderer3_Shutdown();
    renderer_ = nullptr;
}

bool Ui::resume_renderer(SDL_Renderer* renderer) {
    if (!initialised_ || renderer == nullptr || renderer_ != nullptr) {
        return false;
    }
    if (!ImGui_ImplSDLRenderer3_Init(renderer)) return false;
    renderer_ = renderer;
    load_splash_texture();
    return true;
}

void Ui::process_event(const SDL_Event& event) {
    if (!initialised_) return;
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (event.type == SDL_EVENT_FINGER_DOWN ||
        event.type == SDL_EVENT_FINGER_MOTION ||
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        // A touch anywhere wakes the unobtrusive in-game Menu control.  This
        // runs before the on-screen pad consumes the event, so pressing a game
        // button also counts as activity without stealing that press.
        wake_menu_button();
    }
    if (show_launcher_ &&
        (page_ == Page::Library || page_ == Page::Downloads)) {
        if (event.type == SDL_EVENT_FINGER_DOWN && library_scroll_finger_ < 0) {
            library_scroll_finger_ = static_cast<s64>(event.tfinger.fingerID);
            library_scroll_last_y_ = event.tfinger.y;
        } else if (event.type == SDL_EVENT_FINGER_MOTION &&
                   library_scroll_finger_ ==
                       static_cast<s64>(event.tfinger.fingerID)) {
            const float display_h = std::max(1.0f, ImGui::GetIO().DisplaySize.y);
            library_scroll_pending_ +=
                (library_scroll_last_y_ - event.tfinger.y) * display_h;
            library_scroll_last_y_ = event.tfinger.y;
        } else if (event.type == SDL_EVENT_FINGER_UP &&
                   library_scroll_finger_ ==
                       static_cast<s64>(event.tfinger.fingerID)) {
            library_scroll_finger_ = -1;
        }
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        (event.key.scancode == SDL_SCANCODE_ESCAPE ||
         event.key.scancode == SDL_SCANCODE_AC_BACK)) {
        menu_requested_ = true;
    }
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
    if (!initialised_) return intent;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (!splash_complete_) {
        draw_splash();
        if (SDL_GetTicks() - splash_started_ms_ >= 1400u ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            splash_complete_ = true;
        }
    } else if (!setup_complete_) {
        menu_requested_ = false;
        poll_setup_picker(intent);
        draw_setup_wizard(console, intent);
    } else if (show_launcher_) {
        menu_requested_ = false;
        draw_launcher(console, emulating, touch_visible, touch_editing, intent);
    } else {
        if (menu_requested_) {
            if (quick_menu_) {
                quick_menu_ = false;
                if (resume_after_menu_ && !emulating) intent.toggle_pause = true;
            } else {
                open_quick_menu(emulating, intent);
            }
            menu_requested_ = false;
        }
        draw_overlay(console, emulating, display_fps, emulated_fps, frame_ms,
                     underruns, touch_visible, touch_editing, intent);
        if (quick_menu_) {
            draw_quick_menu(console, emulating, touch_visible, touch_editing,
                            intent);
        }
    }

    ImGui::Render();
    return intent;
}

void Ui::draw_splash() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("##splash", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 top = viewport->Pos;
    const ImVec2 bottom(top.x + viewport->Size.x, top.y + viewport->Size.y);
    draw->AddRectFilledMultiColor(top, bottom, IM_COL32(6, 11, 24, 255),
                                  IM_COL32(8, 37, 52, 255),
                                  IM_COL32(22, 8, 42, 255),
                                  IM_COL32(6, 11, 24, 255));

    const ImVec2 centre(top.x + viewport->Size.x * 0.5f,
                        top.y + viewport->Size.y * 0.45f);
    const float icon_size =
        std::min(300.0f * scale_, viewport->Size.y * 0.70f);
    if (splash_texture_ != nullptr) {
        draw->AddImage(
            ImTextureRef(static_cast<ImTextureID>(
                reinterpret_cast<intptr_t>(splash_texture_))),
            ImVec2(centre.x - icon_size * 0.5f,
                   centre.y - icon_size * 0.5f),
            ImVec2(centre.x + icon_size * 0.5f,
                   centre.y + icon_size * 0.5f));
    } else {
        ImGui::SetWindowFontScale(2.55f);
        const char* title = "RETRO-3DO";
        const ImVec2 title_size = ImGui::CalcTextSize(title);
        ImGui::SetCursorScreenPos(ImVec2(centre.x - title_size.x * 0.5f,
                                        centre.y - title_size.y * 0.5f));
        ImGui::TextUnformatted(title);
        ImGui::SetWindowFontScale(1.0f);
    }

    const char* strap = "THE INTERACTIVE MULTIPLAYER, REIMAGINED";
    const ImVec2 strap_size = ImGui::CalcTextSize(strap);
    ImGui::SetCursorScreenPos(ImVec2(centre.x - strap_size.x * 0.5f,
                                    centre.y + icon_size * 0.5f +
                                        10.0f * scale_));
    ImGui::TextColored(kCyan, "%s", strap);
    ImGui::End();
}

void Ui::poll_setup_picker(UiIntent& intent) {
    if (wizard_pick_ == WizardPick::None) return;
    const DocumentEntry picked = AndroidStorage::consume_picked_folder();
    if (picked.uri.empty()) return;

    intent.folder_path = picked.uri;
    intent.folder_name = picked.name;
    if (wizard_pick_ == WizardPick::Bios) {
        bios_folder_name_ = picked.name;
        bios_folder_target_ = picked.uri;
        intent.bios_folder_chosen = true;
    } else {
        games_folder_name_ = picked.name;
        games_folder_target_ = picked.uri;
        intent.games_folder_chosen = true;
    }
    wizard_pick_ = WizardPick::None;
}

void Ui::draw_setup_wizard(Console& console, UiIntent& intent) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##setup_backdrop", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(
        viewport->WorkPos,
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x,
               viewport->WorkPos.y + viewport->WorkSize.y),
        IM_COL32(6, 12, 25, 255), IM_COL32(7, 30, 43, 255),
        IM_COL32(20, 8, 36, 255), IM_COL32(6, 12, 25, 255));

    const float width = std::min(750.0f * scale_, viewport->WorkSize.x * 0.90f);
    const float height = std::min(430.0f * scale_, viewport->WorkSize.y * 0.90f);
    ImGui::SetCursorPos(ImVec2((viewport->WorkSize.x - width) * 0.5f,
                              (viewport->WorkSize.y - height) * 0.5f));
    ImGui::BeginChild("##setup_card", ImVec2(width, height),
                      ImGuiChildFlags_Borders);
    ImGui::TextColored(kCyan, "RETRO-3DO SETUP");
    const int step = wizard_step_ == WizardStep::Welcome    ? 1
                   : wizard_step_ == WizardStep::Bios       ? 2
                   : wizard_step_ == WizardStep::Games      ? 3
                   : wizard_step_ == WizardStep::RetroMedia ? 4
                                                             : 5;
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "STEP %d OF 5", step);
    ImGui::Separator();
    ImGui::Spacing();

    switch (wizard_step_) {
        case WizardStep::Welcome:
            ImGui::SetWindowFontScale(1.55f);
            ImGui::TextUnformatted("WELCOME TO RETRO-3DO");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextWrapped(
                "Setup needs access to two folders: one containing your legally "
                "obtained 3DO BIOS and one containing your games. Android will "
                "show its folder picker for each choice.");
            ImGui::Spacing();
            ImGui::TextColored(kGreen, "SCOPED STORAGE");
            ImGui::TextWrapped(
                "Retro-3DO only receives access to the folders you choose. It "
                "does not request permission to read the rest of your device.");
            ImGui::Spacing();
            if (wide_button("BEGIN SETUP", 44.0f * scale_)) {
                wizard_step_ = WizardStep::Bios;
            }
            break;

        case WizardStep::Bios:
            ImGui::SetWindowFontScale(1.45f);
            ImGui::TextUnformatted("CHOOSE YOUR BIOS FOLDER");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextWrapped(
                "Select the folder containing your 3DO .rom or .bin BIOS. "
                "Retro-3DO will find and validate it automatically.");
            ImGui::Spacing();
            ImGui::TextColored(bios_folder_target_.empty() ? kMuted : kCyan,
                               "%s", bios_folder_target_.empty()
                                         ? "No folder selected"
                                         : bios_folder_name_.c_str());
            if (wide_button(bios_folder_target_.empty() ? "SELECT BIOS FOLDER"
                                                         : "CHANGE BIOS FOLDER",
                            42.0f * scale_)) {
                wizard_pick_ = WizardPick::Bios;
                AndroidStorage::pick_folder();
            }
            if (wizard_pick_ == WizardPick::Bios) {
                ImGui::TextColored(kAmber, "Waiting for Android folder selection...");
            } else if (console.bios_loaded()) {
                ImGui::TextColored(kGreen, "BIOS found and loaded successfully");
            } else if (!bios_folder_target_.empty()) {
                ImGui::TextColored(kRed,
                                   "No valid BIOS was found. Choose another folder.");
            }
            ImGui::BeginDisabled(!console.bios_loaded());
            if (wide_button("CONTINUE", 42.0f * scale_)) {
                wizard_step_ = WizardStep::Games;
            }
            ImGui::EndDisabled();
            break;

        case WizardStep::Games:
            ImGui::SetWindowFontScale(1.45f);
            ImGui::TextUnformatted("CHOOSE YOUR GAMES FOLDER");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextWrapped(
                "Select the top-level folder containing your ISO, BIN/CUE, IMG "
                "or CHD games. Subfolders are scanned automatically.");
            ImGui::Spacing();
            ImGui::TextColored(games_folder_target_.empty() ? kMuted : kCyan,
                               "%s", games_folder_target_.empty()
                                         ? "No folder selected"
                                         : games_folder_name_.c_str());
            if (wide_button(games_folder_target_.empty() ? "SELECT GAMES FOLDER"
                                                          : "CHANGE GAMES FOLDER",
                            42.0f * scale_)) {
                wizard_pick_ = WizardPick::Games;
                AndroidStorage::pick_folder();
            }
            if (wizard_pick_ == WizardPick::Games) {
                ImGui::TextColored(kAmber, "Waiting for Android folder selection...");
            } else if (!games_folder_target_.empty()) {
                ImGui::TextColored(kGreen, "%zu game%s currently in the library",
                                   library_.games().size(),
                                   library_.games().size() == 1 ? "" : "s");
            }
            ImGui::BeginDisabled(games_folder_target_.empty());
            if (wide_button("CONTINUE", 42.0f * scale_)) {
                wizard_step_ = WizardStep::RetroMedia;
            }
            ImGui::EndDisabled();
            break;

        case WizardStep::RetroMedia:
            ImGui::SetWindowFontScale(1.45f);
            ImGui::TextUnformatted("RETROMEDIA (OPTIONAL)");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextWrapped(
                "Sign in to download library artwork. Administrator accounts "
                "also unlock the private game-download catalogue.");
            ImGui::Spacing();
            if (retro_media_email_.empty()) {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##wizard_rm_email", "Email",
                                         retro_media_email_buffer_,
                                         sizeof(retro_media_email_buffer_));
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##wizard_rm_password", "Password",
                                         retro_media_password_buffer_,
                                         sizeof(retro_media_password_buffer_),
                                         ImGuiInputTextFlags_Password);
                ImGui::BeginDisabled(retro_media_busy_);
                if (wide_button("SIGN IN", 42.0f * scale_)) {
                    retro_media_busy_ = true;
                    retro_media_message_ = "Signing in...";
                    intent.retro_media_login = true;
                    intent.retro_media_email = retro_media_email_buffer_;
                    intent.retro_media_password = retro_media_password_buffer_;
                }
                ImGui::EndDisabled();
            } else {
                ImGui::TextColored(kGreen, "SIGNED IN: %s",
                                   retro_media_email_.c_str());
                ImGui::TextColored(retro_media_admin_ ? kAmber : kMuted, "%s",
                    retro_media_admin_ ? "Administrator game catalogue unlocked"
                                       : "Artwork account connected");
            }
            if (!retro_media_message_.empty()) {
                ImGui::TextColored(retro_media_busy_ ? kAmber : kMuted, "%s",
                                   retro_media_message_.c_str());
            }
            ImGui::BeginDisabled(retro_media_busy_);
            if (wide_button(retro_media_email_.empty() ? "SKIP FOR NOW" : "CONTINUE",
                            42.0f * scale_)) {
                wizard_step_ = WizardStep::Finish;
            }
            ImGui::EndDisabled();
            break;

        case WizardStep::Finish:
            ImGui::SetWindowFontScale(1.55f);
            ImGui::TextColored(kGreen, "SETUP COMPLETE");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextWrapped(
                "Your BIOS is ready and %zu game%s %s available. You can change "
                "either folder later from the System page.",
                library_.games().size(), library_.games().size() == 1 ? "" : "s",
                library_.games().size() == 1 ? "is" : "are");
            ImGui::Spacing();
            if (wide_button("OPEN GAME LIBRARY", 46.0f * scale_)) {
                setup_complete_ = true;
                show_launcher_ = true;
                page_ = Page::Library;
                intent.setup_finished = true;
            }
            break;
    }

    ImGui::EndChild();
    ImGui::End();
}

void Ui::draw_launcher(Console& console, bool emulating, bool touch_visible,
                       bool touch_editing, UiIntent& intent) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Retro-3DO", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    const ImVec2 nav_top = ImGui::GetCursorScreenPos();
    const float nav_height = 50.0f * scale_;
    ImGui::GetWindowDrawList()->AddRectFilled(
        nav_top,
        ImVec2(nav_top.x + ImGui::GetContentRegionAvail().x,
               nav_top.y + nav_height),
        IM_COL32(6, 9, 15, 255), 7.0f * scale_);
    ImGui::SetCursorScreenPos(
        ImVec2(nav_top.x, nav_top.y + 5.0f * scale_));
    const float total_width = ImGui::GetContentRegionAvail().x;
    const int action_count = 6 + (retro_media_admin_ ? 1 : 0) +
                             (session_available_ ? 1 : 0);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float action_width =
        std::max(1.0f, (total_width - gap * (action_count - 1)) /
                           static_cast<float>(action_count));

    auto nav_button = [&](const char* label, Page page) {
        const bool selected = page_ == page;
        if (selected) {
            ImVec4 accent = kAmber;
            if (page == Page::Library) accent = kCyan;
            if (page == Page::Settings) accent = kViolet;
            if (page == Page::Artwork) accent = kPink;
            if (page == Page::About) accent = kGreen;
            ImGui::PushStyleColor(ImGuiCol_Button, tinted(accent, 0.38f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  tinted(accent, 0.52f));
        }
        if (ImGui::Button(label, ImVec2(action_width, 40.0f * scale_))) page_ = page;
        if (selected) ImGui::PopStyleColor(2);
    };
    nav_button("LIBRARY", Page::Library);
    ImGui::SameLine();
    nav_button("SETTINGS", Page::Settings);
    ImGui::SameLine();
    nav_button("ARTWORK", Page::Artwork);
    if (retro_media_admin_) {
        ImGui::SameLine();
        nav_button("DOWNLOADS", Page::Downloads);
    }
    ImGui::SameLine();
    nav_button("SYSTEM", Page::System);
    ImGui::SameLine();
    nav_button("ABOUT", Page::About);
    if (session_available_) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.34f, 0.27f, 1.0f));
        if (ImGui::Button("RESUME", ImVec2(action_width, 40.0f * scale_))) {
            show_launcher_ = false;
            wake_menu_button();
            if (!emulating) intent.toggle_pause = true;
        }
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (ImGui::Button("EXIT", ImVec2(action_width, 40.0f * scale_))) {
        intent.quit = true;
    }
    ImGui::SetCursorScreenPos(ImVec2(nav_top.x, nav_top.y + nav_height));
    const ImGuiWindowFlags page_flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::BeginChild("##page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      page_flags);
    switch (page_) {
        case Page::Library:  draw_library(console, intent); break;
        case Page::Settings: draw_settings(console, touch_visible, touch_editing,
                                           intent); break;
        case Page::Artwork:  draw_artwork(intent); break;
        case Page::Downloads: draw_downloads(intent); break;
        case Page::System:   draw_system(console, intent); break;
        case Page::About:    draw_about(); break;
    }
    ImGui::EndChild();
    ImGui::End();

    draw_file_browser(intent);
}

void Ui::draw_library(Console& console, UiIntent& intent) {
    const char groups[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
    auto filter_button = [&](const char* label, char group, float width) {
        const bool selected = library_group_ == group;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.38f, 0.43f, 1.0f));
        }
        if (ImGui::Button(label, ImVec2(width, 0.0f))) {
            library_group_ = selected ? 0 : group;
        }
        if (selected) ImGui::PopStyleColor();
    };

    // A-Z plus one wider 0-9 button, distributed as 28 equal units so the
    // strip reaches both edges at every resolution. Tapping the selected
    // filter again clears it, replacing the need for a separate ALL button.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(3.0f * scale_, ImGui::GetStyle().ItemSpacing.y));
    const float strip_width = ImGui::GetContentRegionAvail().x;
    const float strip_spacing = ImGui::GetStyle().ItemSpacing.x * 26.0f;
    const float unit = std::max(16.0f * scale_,
                                (strip_width - strip_spacing) / 28.0f);
    ImGui::SetWindowFontScale(0.86f);
    bool first_group = true;
    for (char group : groups) {
        if (!first_group) ImGui::SameLine();
        first_group = false;
        char label[2] = {group, '\0'};
        filter_button(group == '#' ? "0-9" : label, group,
                      group == '#' ? unit * 2.0f : unit);
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleVar();
    ImGui::Spacing();

    const float rescan_width = 165.0f * scale_;
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x -
                                              rescan_width - 12.0f * scale_));
    ImGui::InputTextWithHint("##search", "Search your library...", search_buffer_,
                             sizeof(search_buffer_));
    ImGui::SameLine();
    ImGui::BeginDisabled(games_folder_target_.empty());
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.35f, 0.39f, 1.0f));
    if (ImGui::Button("RESCAN GAMES", ImVec2(rescan_width, 0.0f))) {
        intent.rescan_games = true;
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    // The controls above stay fixed. Only the cards are a scrolling surface,
    // and the scrollbar itself is intentionally hidden: touch users swipe the
    // list, while desktop users can still use a wheel or trackpad.
    ImGui::BeginChild("##game_list", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    if (library_scroll_pending_ != 0.0f) {
        ImGui::SetScrollY(std::max(0.0f,
                                   ImGui::GetScrollY() + library_scroll_pending_));
        library_scroll_pending_ = 0.0f;
    }

    const std::vector<const LibraryGame*> games =
        library_.filtered(search_buffer_, library_group_);
    if (games.empty()) {
        ImGui::BeginChild("##empty_library", ImVec2(0.0f, 180.0f * scale_),
                          ImGuiChildFlags_Borders);
        ImGui::Spacing();
        ImGui::SetWindowFontScale(1.25f);
        ImGui::TextColored(kMuted, library_.games().empty()
                                      ? "YOUR LIBRARY IS EMPTY"
                                      : "NO GAMES MATCH THIS FILTER");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextWrapped(library_.games().empty()
                               ? "Put ISO, BIN/CUE, IMG or CHD images in your configured "
                                 "games folder, then choose Rescan Games."
                               : "Try another letter or clear the search box.");
        ImGui::EndChild();
        ImGui::EndChild();
        return;
    }

    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = available >= 720.0f * scale_
                            ? 4
                            : (available >= 540.0f * scale_
                                   ? 3
                                   : (available >= 400.0f * scale_ ? 2 : 1));
    const float card_height = 150.0f * scale_;

    // Nested child windows and table cells both introduce their own clip
    // channels. On Android's high-DPI SDL renderer those channels can lag a
    // row behind, leaving borders visible but clipping the card contents. One
    // dummy reserves the whole scrollable canvas; cards and buttons are then
    // placed directly in that single canvas with no nested clip state.
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float card_width = (available - gap * (columns - 1)) / columns;
    const size_t row_count =
        (games.size() + static_cast<size_t>(columns) - 1) /
        static_cast<size_t>(columns);
    const float grid_height = card_height * static_cast<float>(row_count) +
                              gap * static_cast<float>(row_count - 1);
    const ImVec2 grid_top = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(available, grid_height));
    const ImVec2 grid_end = ImGui::GetCursorScreenPos();
    const ImVec2 list_clip_min = ImGui::GetWindowPos();
    const ImVec2 list_clip_max(list_clip_min.x + ImGui::GetWindowWidth(),
                               list_clip_min.y + ImGui::GetWindowHeight());
    ImDrawList* const list_draw = ImGui::GetWindowDrawList();
    list_draw->PushClipRect(list_clip_min, list_clip_max, true);

    for (size_t index = 0; index < games.size(); ++index) {
            const LibraryGame& game = *games[index];
            const ImVec4 accent = card_accent(game.name);
            ImGui::PushID(game.target.c_str());

            const size_t row = index / static_cast<size_t>(columns);
            const size_t column = index % static_cast<size_t>(columns);
            const ImVec2 card_top(
                grid_top.x + static_cast<float>(column) * (card_width + gap),
                grid_top.y + static_cast<float>(row) * (card_height + gap));
            const ImVec2 card_bottom(card_top.x + card_width,
                                     card_top.y + card_height);
            ImDrawList* draw = list_draw;
            draw->AddRectFilled(card_top, card_bottom,
                                colour(tinted(accent, 0.045f)),
                                ImGui::GetStyle().ChildRounding);
            draw->AddRect(card_top, card_bottom,
                          IM_COL32(static_cast<int>(accent.x * 132.0f),
                                   static_cast<int>(accent.y * 132.0f),
                                   static_cast<int>(accent.z * 132.0f), 224),
                          ImGui::GetStyle().ChildRounding, 0,
                          std::max(1.0f, scale_));
            draw->AddRectFilled(
                card_top, ImVec2(card_top.x + 4.0f * scale_, card_bottom.y),
                colour(accent), ImGui::GetStyle().ChildRounding,
                ImDrawFlags_RoundCornersLeft);

            const float padding = 10.0f * scale_;
            const float content_left = card_top.x + padding;
            const float content_top = card_top.y + padding;
            const float art_height = 76.0f * scale_;
            SDL_Texture* cover = artwork_texture(game.name);
            if (cover != nullptr) {
                const ArtworkTexture& item = artwork_.find(game.name)->second;
                const float aspect = static_cast<float>(item.width) /
                                     static_cast<float>(std::max(1, item.height));
                const float art_width = std::clamp(art_height * aspect,
                                                   62.0f * scale_,
                                                   108.0f * scale_);
                const float art_left =
                    card_top.x + (card_width - art_width) * 0.5f;
                draw->AddImage(
                    ImTextureRef(static_cast<ImTextureID>(
                        reinterpret_cast<intptr_t>(cover))),
                    ImVec2(art_left, content_top),
                    ImVec2(art_left + art_width, content_top + art_height));
            } else {
                const float tile_width = 76.0f * scale_;
                const float tile_left =
                    card_top.x + (card_width - tile_width) * 0.5f;
                draw->AddRectFilled(
                    ImVec2(tile_left, content_top),
                    ImVec2(tile_left + tile_width, content_top + art_height),
                    colour(tinted(accent, 0.16f)), 6.0f * scale_);
                const char initial[2] = {card_initial(game.name), '\0'};
                const float initial_size = ImGui::GetFontSize() * 1.7f;
                const ImVec2 initial_bounds = ImGui::GetFont()->CalcTextSizeA(
                    initial_size, FLT_MAX, 0.0f, initial);
                draw->AddText(
                    ImGui::GetFont(), initial_size,
                    ImVec2(tile_left +
                               (tile_width - initial_bounds.x) * 0.5f,
                           content_top +
                               (art_height - initial_bounds.y) * 0.5f),
                    colour(accent), initial);
            }

            const float text_width = card_width - padding * 2.0f;
            draw_card_title(draw, game.name,
                            ImVec2(content_left,
                                   content_top + art_height + 6.0f * scale_),
                            text_width, IM_COL32(232, 240, 247, 255),
                            ImGui::GetFontSize() * 0.92f);
            const float button_height = ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(
                ImVec2(content_left,
                       card_bottom.y - padding - button_height));
            ImGui::BeginDisabled(!console.bios_loaded());
            ImGui::PushStyleColor(ImGuiCol_Button, tinted(accent, 0.13f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  tinted(accent, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  tinted(accent, 0.46f));
            if (ImGui::Button("PLAY",
                              ImVec2(card_width - padding * 2.0f, 0.0f))) {
                intent.disc_chosen = true;
                intent.start_disc = true;
                intent.disc_path = game.target;
                intent.disc_name = game.name;
                active_discs_ = game.discs;
                active_disc_ = 0;
                session_available_ = true;
                show_launcher_ = false;
                wake_menu_button();
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();

            if (game.discs.size() > 1) {
                const std::string badge = std::to_string(game.discs.size()) + " DISCS";
                const ImVec2 badge_size = ImGui::GetFont()->CalcTextSizeA(
                    ImGui::GetFontSize() * 0.72f, FLT_MAX, 0.0f, badge.c_str());
                const ImVec2 badge_min(card_bottom.x - badge_size.x -
                                           16.0f * scale_,
                                       card_top.y + 7.0f * scale_);
                draw->AddRectFilled(
                    ImVec2(badge_min.x - 5.0f * scale_, badge_min.y - 2.0f * scale_),
                    ImVec2(card_bottom.x - 6.0f * scale_,
                           badge_min.y + badge_size.y + 2.0f * scale_),
                    IM_COL32(12, 20, 34, 230), 4.0f * scale_);
                draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.72f,
                              badge_min, colour(kAmber), badge.c_str());
            }
            ImGui::PopID();
    }
    list_draw->PopClipRect();
    ImGui::SetCursorScreenPos(grid_end);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    if (!console.bios_loaded()) {
        ImGui::Spacing();
        ImGui::TextColored(kAmber, "Load a 3DO BIOS on the System page before playing.");
    }
    ImGui::EndChild();
}

void Ui::draw_settings(Console& console, bool touch_visible, bool touch_editing,
                       UiIntent& intent) {
    page_heading("SETTINGS", "Controls and machine timing");

    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float column_width = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(7.0f * scale_, 5.0f * scale_));

    ImGui::BeginChild("##controls_settings", ImVec2(column_width, 0.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(kBlue, "ON-SCREEN CONTROLLER");
    ImGui::TextColored(kMuted, "Automatically hidden while a physical pad is connected.");
    if (ImGui::Button(touch_visible ? "ON - HIDE" : "OFF - SHOW",
                      ImVec2(165.0f * scale_, 0.0f))) {
        intent.toggle_touch_controls = true;
    }
    if (touch_visible) {
        ImGui::SameLine();
        if (ImGui::Button(touch_editing ? "DONE MOVING" : "EDIT LAYOUT")) {
            intent.toggle_layout_edit = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("RESET")) intent.reset_touch_layout = true;
    }

    ImGui::Separator();
    ImGui::TextColored(kGreen, "VIDEO STANDARD");
    int region = console.region() == Region::Pal ? 1 : 0;
    if (ImGui::RadioButton("NTSC 60 Hz", &region, 0)) {
        intent.region_changed = true;
        intent.set_region_pal = false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("PAL 50 Hz", &region, 1)) {
        intent.region_changed = true;
        intent.set_region_pal = true;
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##machine_settings", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(kAmber, "ARM CPU BOOST");
    ImGui::TextColored(kMuted, "Game CPU only; audio, video and FMV keep native timing.");
    int cpu = static_cast<int>(console.cpu_scale_percent());
    const int speeds[] = {100, 125, 150};
    const char* speed_names[] = {"100%", "125%", "150%"};
    for (int i = 0; i < 3; ++i) {
        if (i != 0) ImGui::SameLine();
        if (ImGui::RadioButton(speed_names[i], &cpu, speeds[i])) {
            intent.cpu_scale_changed = true;
            intent.cpu_scale_percent = static_cast<u32>(speeds[i]);
        }
    }

    ImGui::Separator();
    ImGui::TextColored(kPink, "PERFORMANCE DISPLAY");
    if (ImGui::Checkbox("Show in-game performance HUD", &performance_visible_)) {
        intent.ui_settings_changed = true;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void Ui::draw_artwork(UiIntent& intent) {
    page_heading("ARTWORK", "RetroMedia account and library card downloads");

    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float column_width = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(7.0f * scale_, 7.0f * scale_));

    ImGui::BeginChild("##retromedia_account", ImVec2(column_width, 0.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(kViolet, "RETROMEDIA ACCOUNT");
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped(
        "Sign in with your registered account. Retro-3DO stores only the "
        "encrypted session; passwords are never saved.");
    ImGui::PopStyleColor();

    if (!AndroidStorage::available()) {
        ImGui::TextColored(kAmber,
                           "Account downloads are available in the Android build.");
    } else if (retro_media_email_.empty()) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##rm_email", "Registered email",
                                 retro_media_email_buffer_,
                                 sizeof(retro_media_email_buffer_));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##rm_password", "Password",
                                 retro_media_password_buffer_,
                                 sizeof(retro_media_password_buffer_),
                                 ImGuiInputTextFlags_Password);
        ImGui::BeginDisabled(retro_media_busy_);
        if (ImGui::Button("SIGN IN", ImVec2(-1.0f, 0.0f))) {
            retro_media_busy_ = true;
            retro_media_message_ = "Signing in...";
            intent.retro_media_login = true;
            intent.retro_media_email = retro_media_email_buffer_;
            intent.retro_media_password = retro_media_password_buffer_;
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextColored(kGreen, "%s", retro_media_email_.c_str());
        ImGui::Text("Daily downloads left: %d    Credits: %d",
                    retro_media_free_remaining_, retro_media_credits_);
        ImGui::BeginDisabled(retro_media_busy_);
        if (ImGui::Button("SIGN OUT", ImVec2(-1.0f, 0.0f))) {
            retro_media_busy_ = true;
            retro_media_message_ = "Signing out...";
            intent.retro_media_logout = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##retromedia_downloads", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(kCyan, "GAME CARD ARTWORK");
    ImGui::TextColored(kMuted, "%zu game%s in this library",
                       library_.games().size(),
                       library_.games().size() == 1 ? "" : "s");
    const char* type_ids[] = {"box2d", "images", "thumbnails", "titles"};
    const char* type_names[] = {"2D COVER", "SCREENSHOT", "THUMBNAIL",
                                "TITLE SCREEN"};
    int selected_type = 0;
    for (int i = 0; i < 4; ++i) {
        if (retro_media_type_ == type_ids[i]) selected_type = i;
    }
    ImGui::TextColored(kMuted, "Choose what appears on every library card:");
    for (int i = 0; i < 4; ++i) {
        if (i == 1 || i == 3) ImGui::SameLine();
        if (ImGui::RadioButton(type_names[i], &selected_type, i)) {
            retro_media_type_ = type_ids[i];
            intent.retro_media_artwork_changed = true;
            intent.ui_settings_changed = true;
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped(
        "Only missing cards are downloaded. Daily allowance and credits apply.");
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(retro_media_busy_ || retro_media_email_.empty() ||
                         library_.games().empty());
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.07f, 0.40f, 0.42f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.09f, 0.55f, 0.57f, 1.0f));
    if (ImGui::Button("SYNC / DOWNLOAD MISSING ARTWORK",
                      ImVec2(-1.0f, 46.0f * scale_))) {
        retro_media_busy_ = true;
        retro_media_message_ = "Matching library and downloading artwork...";
        intent.retro_media_sync = true;
    }
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    ImGui::TextColored(retro_media_busy_ ? kAmber : kMuted, "%s",
                       retro_media_message_.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void Ui::draw_downloads(UiIntent& intent) {
    page_heading("GAME DOWNLOADS", "Administrator RetroMedia catalogue");
    if (!retro_media_admin_) {
        ImGui::TextColored(kAmber,
                           "This page requires a RetroMedia administrator account.");
        return;
    }
    const char groups[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(3.0f * scale_, ImGui::GetStyle().ItemSpacing.y));
    const float strip_width = ImGui::GetContentRegionAvail().x;
    const float strip_spacing = ImGui::GetStyle().ItemSpacing.x * 26.0f;
    const float unit = std::max(16.0f * scale_,
                                (strip_width - strip_spacing) / 28.0f);
    ImGui::SetWindowFontScale(0.86f);
    bool first = true;
    for (char group : groups) {
        if (!first) ImGui::SameLine();
        first = false;
        char label[2] = {group, '\0'};
        const bool selected = retro_media_group_ == group;
        if (selected) ImGui::PushStyleColor(
            ImGuiCol_Button, ImVec4(0.38f, 0.25f, 0.12f, 1.0f));
        if (ImGui::Button(group == '#' ? "0-9" : label,
                          ImVec2(group == '#' ? unit * 2.0f : unit, 0.0f))) {
            retro_media_group_ = selected ? 0 : group;
        }
        if (selected) ImGui::PopStyleColor();
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleVar();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190.0f * scale_);
    ImGui::InputTextWithHint("##rm_catalogue_search", "Search available games...",
                             retro_media_search_buffer_,
                             sizeof(retro_media_search_buffer_));
    ImGui::SameLine();
    ImGui::BeginDisabled(retro_media_busy_);
    if (ImGui::Button("BROWSE / REFRESH", ImVec2(-1.0f, 0.0f))) {
        retro_media_busy_ = true;
        retro_media_message_ = "Loading administrator catalogue...";
        intent.retro_media_catalogue = true;
        intent.retro_media_search = retro_media_search_buffer_;
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextColored(retro_media_busy_ ? kAmber : kMuted, "%s",
                       retro_media_message_.c_str());
    ImGui::BeginChild("##rm_game_catalogue", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    if (library_scroll_pending_ != 0.0f) {
        ImGui::SetScrollY(std::max(
            0.0f, ImGui::GetScrollY() + library_scroll_pending_));
        library_scroll_pending_ = 0.0f;
    }
    if (retro_media_catalogue_.empty()) {
        ImGui::TextWrapped(
            "Choose Browse / Refresh to list ROM sets available to this "
            "administrator. Downloads go to the selected Games folder.");
    }
    std::vector<const CatalogueItem*> shown;
    for (const CatalogueItem& game : retro_media_catalogue_) {
        char head = 0;
        for (unsigned char c : game.name) {
            if (std::isdigit(c)) { head = '#'; break; }
            if (std::isalpha(c)) {
                head = static_cast<char>(std::toupper(c));
                break;
            }
        }
        if (retro_media_group_ == 0 || head == retro_media_group_) {
            shown.push_back(&game);
        }
    }
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = available >= 720.0f * scale_ ? 4
                        : available >= 500.0f * scale_ ? 3 : 2;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float width = (available - gap * (columns - 1)) / columns;
    for (size_t index = 0; index < shown.size(); ++index) {
        const CatalogueItem& game = *shown[index];
        if (index % static_cast<size_t>(columns) != 0) ImGui::SameLine();
        ImGui::PushID(game.slug.c_str());
        ImGui::BeginChild("##rm_game", ImVec2(width, 84.0f * scale_),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextWrapped("%.36s", game.name.c_str());
        const double mib = static_cast<double>(game.total_bytes) / (1024.0 * 1024.0);
        ImGui::TextColored(kMuted, "%d file%s  |  %.1f MiB", game.rom_files,
                           game.rom_files == 1 ? "" : "s", mib);
        ImGui::BeginDisabled(retro_media_busy_ || games_folder_target_.empty());
        if (ImGui::Button("DOWNLOAD", ImVec2(-1.0f, 0.0f))) {
            retro_media_busy_ = true;
            retro_media_message_ = "Downloading " + game.name + "...";
            intent.retro_media_download = true;
            intent.retro_media_slug = game.slug;
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void Ui::draw_system(Console& console, UiIntent& intent) {
    page_heading("SYSTEM", "Renderer, screen presentation and storage");

    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float column_width = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(7.0f * scale_, 5.0f * scale_));

    ImGui::BeginChild("##graphics_system", ImVec2(column_width, 0.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(kBlue, "RENDERER");
    int renderer = renderer_backend_;
    if (ImGui::RadioButton("AUTO", &renderer, 0)) {
        renderer_backend_ = 0;
        renderer_startup_message_ = "Renderer change applies next launch";
        intent.ui_settings_changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("VULKAN", &renderer, 1)) {
        renderer_backend_ = 1;
        renderer_startup_message_ = "Renderer change applies next launch";
        intent.ui_settings_changed = true;
    }
    ImGui::TextColored(kMuted, "Active: %s",
                       actual_renderer_.empty() ? "detecting" : actual_renderer_.c_str());
    if (!renderer_startup_message_.empty()) {
        ImGui::TextColored(kAmber, "%s", renderer_startup_message_.c_str());
    }

    ImGui::Separator();
    ImGui::TextColored(kViolet, "CUSTOM ADRENO / TURNIP DRIVER");
    ImGui::TextColored(gpu_driver_name_.empty() ? kMuted : kGreen, "%s",
                       gpu_driver_name_.empty() ? "System GPU driver"
                                                : gpu_driver_name_.c_str());
    if (AndroidStorage::available()) {
        if (ImGui::Button("IMPORT DRIVER PACKAGE", ImVec2(-1.0f, 0.0f))) {
            intent.import_gpu_driver = true;
        }
        if (!gpu_driver_name_.empty()) {
            if (ImGui::Button("USE SYSTEM DRIVER", ImVec2(-1.0f, 0.0f))) {
                intent.use_system_gpu_driver = true;
            }
        }
        ImGui::TextColored(kMuted,
                           "ADPKG/ZIP is copied to private app storage. Restart to apply.");
    } else {
        ImGui::TextColored(kMuted, "Custom mobile drivers are Android-only.");
    }
    if (!gpu_driver_message_.empty()) {
        ImGui::TextColored(kAmber, "%s", gpu_driver_message_.c_str());
    }

    ImGui::Separator();
    ImGui::TextColored(kPink, "DISPLAY");
    int aspect = widescreen_ ? 1 : 0;
    if (ImGui::RadioButton("4:3 ORIGINAL", &aspect, 0)) {
        widescreen_ = false;
        intent.ui_settings_changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("16:9 WIDE", &aspect, 1)) {
        widescreen_ = true;
        intent.ui_settings_changed = true;
    }
    if (ImGui::Checkbox("BEZEL", &bezel_)) intent.ui_settings_changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("CRT EFFECT", &crt_effect_)) {
        intent.ui_settings_changed = true;
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##storage_system", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(kAmber, "3DO SYSTEM ROM");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##bios", "ROM or BIN path", bios_path_buffer_,
                                 sizeof(bios_path_buffer_))) {
        bios_target_.clear();
    }
    if (ImGui::Button("BROWSE##bios", ImVec2(column_width * 0.48f, 0.0f))) {
        browsing_ = Browsing::Bios;
        browser_.open("Choose a BIOS image", {".rom", ".bin"});
    }
    ImGui::SameLine();
    if (ImGui::Button("LOAD##bios", ImVec2(-1.0f, 0.0f))) {
        intent.bios_chosen = true;
        intent.bios_path = bios_target_.empty() ? std::string(bios_path_buffer_)
                                                 : bios_target_;
        intent.bios_name = bios_path_buffer_;
    }
    ImGui::TextColored(console.bios_loaded() ? kGreen : kAmber,
                       console.bios_loaded() ? "BIOS loaded - system ready"
                                             : "No BIOS loaded");

    ImGui::Separator();
    ImGui::TextColored(kGreen, "INSERTED DISC");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##disc", "ISO, BIN/CUE, IMG or CHD",
                                 disc_path_buffer_, sizeof(disc_path_buffer_))) {
        disc_target_.clear();
    }
    if (ImGui::Button("BROWSE##disc", ImVec2(column_width * 0.48f, 0.0f))) {
        browsing_ = Browsing::Disc;
        browser_.open("Choose a disc image", {".iso", ".bin", ".cue", ".img", ".chd"});
    }
    ImGui::SameLine();
    if (ImGui::Button("INSERT##disc", ImVec2(-1.0f, 0.0f))) {
        intent.disc_chosen = true;
        intent.disc_path = disc_target_.empty() ? std::string(disc_path_buffer_)
                                                 : disc_target_;
        intent.disc_name = disc_path_buffer_;
    }
    if (console.disc_loaded()) {
        const Disc& disc = console.disc();
        ImGui::TextColored(kGreen, "Disc ready - %u sectors / %zu track%s",
                           disc.sector_count(), disc.tracks().size(),
                           disc.tracks().size() == 1 ? "" : "s");
        if (ImGui::Button("EJECT")) intent.eject = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(!console.bios_loaded());
        if (ImGui::Button("START CURRENT DISC")) {
            intent.reset = true;
            session_available_ = true;
            show_launcher_ = false;
            wake_menu_button();
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextColored(kMuted, "No disc inserted");
    }

    ImGui::Separator();
    ImGui::TextColored(kCyan, "STORAGE FOLDERS");
    ImGui::Text("BIOS: %.42s", bios_folder_name_.empty()
                                   ? "Not configured" : bios_folder_name_.c_str());
    ImGui::Text("Games: %.41s", games_folder_name_.empty()
                                    ? "Not configured" : games_folder_name_.c_str());
    if (ImGui::Button("RUN FOLDER SETUP", ImVec2(-1.0f, 0.0f))) {
        setup_complete_ = false;
        wizard_step_ = WizardStep::Welcome;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void Ui::draw_about() {
    page_heading("ABOUT RETRO-3DO", "A new, mobile-first 3DO emulator");

    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float column_width = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(7.0f * scale_, 7.0f * scale_));

    ImGui::BeginChild("##about_story", ImVec2(column_width, 0.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextColored(kCyan, "RETRO-3DO");
    ImGui::SetWindowFontScale(0.88f);
    ImGui::TextColored(kMuted, "Version 2.1.0  |  September 2026");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Retro-3DO is a brand-new 3DO emulator implementation, designed and "
        "built as its own modern C++ codebase for phones, handhelds, tablets "
        "and desktops. It is not a re-skinned legacy core.");
    ImGui::Spacing();
    ImGui::TextColored(kGreen, "BUILT FOR MODERN HARDWARE");
    ImGui::TextWrapped(
        "The ARM60 CPU uses a decoded-instruction cache, emulation runs on a "
        "dedicated paced thread, and rendering is presented independently. "
        "Android supports Vulkan, custom Adreno drivers, scoped storage and "
        "up to 120 Hz display presentation without changing game or audio speed.");
    ImGui::Spacing();
    ImGui::TextColored(kAmber, "ACTIVE DEVELOPMENT");
    ImGui::TextWrapped(
        "Commercial games including Road Rash and The Need for Speed reach "
        "correctly rendered gameplay. Compatibility continues to expand; "
        "save states, CD audio and less-used hardware paths remain future work.");
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##about_technology", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowFontScale(0.88f);
    ImGui::TextColored(kViolet, "THE EMULATED SYSTEM");
    ImGui::TextWrapped(
        "Project implementations cover the ARM60, CLIO timing and interrupts, "
        "MADAM/CEL graphics and matrix hardware, VDLP display lists, DSPP "
        "audio, SPORT memory operations, XBUS CD-ROM and chained controllers.");
    ImGui::Spacing();
    ImGui::TextColored(kGreen, "LICENCE & OWNERSHIP");
    ImGui::TextWrapped(
        "Project-owned code: MIT License. Copyright (c) 2026 Crown Park "
        "Computing. MIT permits use, modification, distribution and sale; "
        "bundled components retain their own permissive licences.");
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped(
        "Licence notices do not replace third-party rights clearance; see "
        "NOTICE and PROVENANCE.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::TextColored(kBlue, "DEVELOPED WITH EVIDENCE");
    ImGui::TextWrapped(
        "The core is backed by 250+ focused regression checks, commercial-game "
        "traces, official 3DO and ARM documentation, published hardware patents "
        "and device testing. Opera/FreeDO was also consulted historically as a "
        "compatibility reference; the project does not claim clean-room history.");
    ImGui::Spacing();
    ImGui::TextColored(kPink, "INCLUDED FEATURES");
    ImGui::TextWrapped(
        "ISO/BIN/CUE/IMG/CHD | live multi-disc swap | gamepad and touch "
        "controls | RetroMedia artwork/admin catalogue | shared mobile and "
        "desktop core");
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void Ui::open_quick_menu(bool emulating, UiIntent& intent) {
    quick_menu_ = true;
    resume_after_menu_ = emulating;
    if (emulating) intent.toggle_pause = true;
}

void Ui::draw_overlay(Console& console, bool emulating, double display_fps,
                      double emulated_fps, double frame_ms, u64 underruns,
                      bool touch_visible, bool touch_editing, UiIntent& intent) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr u64 kMenuVisibleMs = 3000;
    const bool show_menu_button =
        touch_editing ||
        SDL_GetTicks() - last_pointer_activity_ms_ < kMenuVisibleMs;
    if (show_menu_button && !quick_menu_) {
        // Top-centre is deliberately outside both shoulder-button hit areas.
        // The old top-left position covered L on handheld layouts.
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                   viewport->WorkPos.y + 12.0f * scale_),
            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGui::Begin("##menu_button", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
        if (ImGui::Button("MENU", ImVec2(92.0f * scale_, 0.0f))) {
            open_quick_menu(emulating, intent);
        }
        if (touch_editing) {
            ImGui::SameLine();
            if (ImGui::Button("DONE MOVING")) intent.toggle_layout_edit = true;
        }
        ImGui::End();
    }

    if (!performance_visible_) return;

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f * scale_,
                                  viewport->WorkPos.y + 68.0f * scale_));
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##performance", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoNav);
    const double target = console.region() == Region::Pal ? 50.0 : 60.0;
    ImGui::TextColored(emulated_fps >= target - 2.0 ? kGreen : kAmber,
                       "Machine %.0f / %.0f fps", emulated_fps, target);
    ImGui::Text("Display %.0f fps   Frame %.2f ms", display_fps, frame_ms);
    ImGui::Text("ARM %u%%   PC %08X", console.cpu_scale_percent(),
                console.cpu().pc());
    if (underruns > 0) {
        ImGui::TextColored(kRed, "Audio gaps %llu",
                           static_cast<unsigned long long>(underruns));
    }
    ImGui::End();
}

void Ui::draw_quick_menu(Console& console, bool emulating, bool touch_visible,
                         bool touch_editing, UiIntent& intent) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x,
               viewport->Pos.y + viewport->Size.y),
        IM_COL32(2, 5, 12, 180));

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                  viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(
        ImVec2(std::min(465.0f * scale_, viewport->WorkSize.x * 0.88f),
               std::min(510.0f * scale_, viewport->WorkSize.y * 0.92f)),
        ImGuiCond_Always);
    ImGui::Begin("##quick_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextColored(kCyan, "GAME MENU");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextColored(kMuted, "Game paused - machine timing is preserved");
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.35f, 0.28f, 1.0f));
    if (wide_button("RESUME", 42.0f * scale_)) {
        quick_menu_ = false;
        if (resume_after_menu_ && !emulating) intent.toggle_pause = true;
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::TextColored(kCyan, "ON-SCREEN CONTROLLER");
    if (ImGui::Checkbox("Enabled", &touch_visible)) {
        intent.toggle_touch_controls = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!touch_visible);
    if (ImGui::Button(touch_editing ? "DONE MOVING" : "EDIT LAYOUT")) {
        intent.toggle_layout_edit = true;
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextColored(kCyan, "ARM CPU BOOST");
    int cpu = static_cast<int>(console.cpu_scale_percent());
    const int values[] = {100, 125, 150};
    const char* labels[] = {"100%", "125%", "150%"};
    for (int i = 0; i < 3; ++i) {
        if (i != 0) ImGui::SameLine();
        if (ImGui::RadioButton(labels[i], &cpu, values[i])) {
            intent.cpu_scale_changed = true;
            intent.cpu_scale_percent = static_cast<u32>(values[i]);
        }
    }
    ImGui::TextColored(kMuted, "Audio, video and FMV remain at native speed.");

    if (active_discs_.size() > 1) {
        ImGui::Spacing();
        ImGui::TextColored(kAmber, "DISC SELECT");
        for (size_t i = 0; i < active_discs_.size(); ++i) {
            if (i != 0) ImGui::SameLine();
            const std::string label = "DISC " + std::to_string(i + 1);
            ImGui::BeginDisabled(i == active_disc_);
            if (ImGui::Button(label.c_str())) {
                intent.disc_chosen = true;
                intent.disc_path = active_discs_[i].target;
                intent.disc_name = active_discs_[i].name;
                active_disc_ = i;
            }
            ImGui::EndDisabled();
        }
        ImGui::TextColored(kMuted,
                           "Swap when the game asks; the 3DO is not reset.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("RESET GAME", ImVec2(-1.0f, 0.0f))) {
        intent.reset = true;
        quick_menu_ = false;
    }
    if (ImGui::Button("RETURN TO LIBRARY", ImVec2(-1.0f, 0.0f))) {
        show_launcher_ = true;
        quick_menu_ = false;
    }
    if (ImGui::Button("EXIT RETRO-3DO", ImVec2(-1.0f, 0.0f))) intent.quit = true;
    ImGui::End();
}

void Ui::draw_file_browser(UiIntent& intent) {
    std::string picked;
    std::string picked_name;
    if (!browser_.draw(&picked, &picked_name)) return;

    if (browsing_ == Browsing::Bios) {
        std::snprintf(bios_path_buffer_, sizeof(bios_path_buffer_), "%s",
                      picked_name.empty() ? picked.c_str() : picked_name.c_str());
        bios_target_ = picked;
        intent.bios_chosen = true;
        intent.bios_path = picked;
        intent.bios_name = picked_name;
    } else if (browsing_ == Browsing::Disc) {
        std::snprintf(disc_path_buffer_, sizeof(disc_path_buffer_), "%s",
                      picked_name.empty() ? picked.c_str() : picked_name.c_str());
        disc_target_ = picked;
        intent.disc_chosen = true;
        intent.disc_path = picked;
        intent.disc_name = picked_name;
    }
    browsing_ = Browsing::None;
}

void Ui::render(SDL_Renderer* renderer) {
    if (!initialised_) return;
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

}  // namespace retro3do
