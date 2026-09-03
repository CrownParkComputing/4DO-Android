// Retro-3DO front end: splash, library, settings and in-game quick menu.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "ui/file_browser.h"
#include "ui/game_library.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
union SDL_Event;

namespace retro3do {

class Console;
struct RetroMediaArtwork;
struct RetroMediaGame;

// What the UI asks the application to do. It is consumed once per display
// frame, keeping storage and machine mutations out of the drawing code.
struct UiIntent {
    bool quit = false;
    bool reset = false;
    bool start_demo = false;
    bool toggle_pause = false;
    bool bios_chosen = false;
    std::string bios_path;
    std::string bios_name;
    bool disc_chosen = false;
    bool start_disc = false;
    std::string disc_path;
    std::string disc_name;
    bool eject = false;
    bool toggle_touch_controls = false;
    bool toggle_layout_edit = false;
    bool reset_touch_layout = false;
    bool region_changed = false;
    bool set_region_pal = false;
    bool cpu_scale_changed = false;
    u32 cpu_scale_percent = 100;
    bool ui_settings_changed = false;
    bool bios_folder_chosen = false;
    bool games_folder_chosen = false;
    bool setup_finished = false;
    bool rescan_games = false;
    bool import_gpu_driver = false;
    bool use_system_gpu_driver = false;
    bool retro_media_login = false;
    bool retro_media_logout = false;
    bool retro_media_sync = false;
    bool retro_media_artwork_changed = false;
    bool retro_media_catalogue = false;
    bool retro_media_download = false;
    std::string retro_media_email;
    std::string retro_media_password;
    std::string retro_media_search;
    std::string retro_media_slug;
    std::string folder_path;
    std::string folder_name;
};

class Ui {
public:
    Ui();
    ~Ui();

    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    bool init(SDL_Window* window, SDL_Renderer* renderer);
    void hide_launcher();
    void show_launcher() { show_launcher_ = true; }

    void set_remembered_bios(const std::string& name, const std::string& target);
    void set_remembered_disc(const std::string& name, const std::string& target);
    void set_game_library(const std::vector<LibraryGame>& games);
    void configure_setup(bool complete, const std::string& bios_folder_name,
                         const std::string& bios_folder_target,
                         const std::string& games_folder_name,
                         const std::string& games_folder_target);
    void set_performance_visible(bool visible) { performance_visible_ = visible; }
    bool performance_visible() const { return performance_visible_; }
    void configure_video(int renderer_backend, bool widescreen, bool bezel,
                         bool crt_effect, const std::string& gpu_driver_name);
    void set_renderer_status(const std::string& actual,
                             const std::string& startup_message);
    void set_gpu_driver_status(const std::string& name,
                               const std::string& message);
    void configure_retro_media(const std::string& media_type);
    void set_retro_media_status(const std::string& email, int credits,
                                int free_remaining, bool is_admin, bool busy,
                                const std::string& message);
    void set_retro_media_error(const std::string& message);
    void set_retro_media_email_hint(const std::string& email);
    void set_retro_media_artwork(
        const std::vector<RetroMediaArtwork>& artwork);
    void set_retro_media_catalogue(const std::vector<RetroMediaGame>& games);
    int renderer_backend() const { return renderer_backend_; }
    bool widescreen() const { return widescreen_; }
    bool bezel() const { return bezel_; }
    bool crt_effect() const { return crt_effect_; }
    const std::string& gpu_driver_name() const { return gpu_driver_name_; }
    const std::string& retro_media_type() const { return retro_media_type_; }
    void suspend_renderer();
    bool resume_renderer(SDL_Renderer* renderer);
    void shutdown();

    void process_event(const SDL_Event& event);
    bool wants_mouse() const;
    bool wants_keyboard() const;

    UiIntent build(Console& console, bool emulating, double display_fps,
                   double emulated_fps, double frame_ms, u64 underruns,
                   bool touch_visible, bool touch_editing);
    void render(SDL_Renderer* renderer);

private:
    enum class Page { Library, Settings, Artwork, Downloads, System, About };
    enum class Browsing { None, Bios, Disc };
    enum class WizardStep { Welcome, Bios, Games, RetroMedia, Finish };
    enum class WizardPick { None, Bios, Games };

    void draw_splash();
    void draw_setup_wizard(Console& console, UiIntent& intent);
    void poll_setup_picker(UiIntent& intent);
    void draw_launcher(Console& console, bool emulating, bool touch_visible,
                       bool touch_editing, UiIntent& intent);
    void draw_library(Console& console, UiIntent& intent);
    void draw_settings(Console& console, bool touch_visible, bool touch_editing,
                       UiIntent& intent);
    void draw_artwork(UiIntent& intent);
    void draw_downloads(UiIntent& intent);
    void draw_system(Console& console, UiIntent& intent);
    void draw_about();
    void draw_overlay(Console& console, bool emulating, double display_fps,
                      double emulated_fps, double frame_ms, u64 underruns,
                      bool touch_visible, bool touch_editing, UiIntent& intent);
    void draw_quick_menu(Console& console, bool emulating, bool touch_visible,
                         bool touch_editing, UiIntent& intent);
    void open_quick_menu(bool emulating, UiIntent& intent);
    void wake_menu_button();
    void draw_file_browser(UiIntent& intent);
    void load_splash_texture();
    SDL_Texture* artwork_texture(const std::string& game_name);
    void release_artwork_textures();
    void clear_artwork_textures();

    struct ArtworkTexture {
        std::string path;
        int width = 0;
        int height = 0;
        SDL_Texture* texture = nullptr;
        bool attempted = false;
    };

    FileBrowser browser_;
    GameLibrary library_;
    Browsing browsing_ = Browsing::None;
    Page page_ = Page::Library;

    float scale_ = 1.0f;
    bool initialised_ = false;
    bool show_launcher_ = true;
    bool setup_complete_ = true;
    bool session_available_ = false;
    bool splash_complete_ = false;
    bool quick_menu_ = false;
    bool resume_after_menu_ = false;
    bool performance_visible_ = false;
    int renderer_backend_ = 0;
    bool widescreen_ = false;
    bool bezel_ = false;
    bool crt_effect_ = false;
    bool menu_requested_ = false;
    u64 splash_started_ms_ = 0;
    u64 last_pointer_activity_ms_ = 0;
    s64 library_scroll_finger_ = -1;
    float library_scroll_last_y_ = 0.0f;
    float library_scroll_pending_ = 0.0f;
    char search_buffer_[128] = {};
    char library_group_ = 0;
    WizardStep wizard_step_ = WizardStep::Welcome;
    WizardPick wizard_pick_ = WizardPick::None;
    std::string bios_folder_name_;
    std::string bios_folder_target_;
    std::string games_folder_name_;
    std::string games_folder_target_;
    std::string actual_renderer_;
    std::string renderer_startup_message_;
    std::string gpu_driver_name_;
    std::string gpu_driver_message_;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* splash_texture_ = nullptr;
    std::unordered_map<std::string, ArtworkTexture> artwork_;
    std::string retro_media_type_ = "box2d";
    std::string retro_media_email_;
    std::string retro_media_message_ = "Not signed in";
    int retro_media_credits_ = 0;
    int retro_media_free_remaining_ = 0;
    bool retro_media_busy_ = false;
    bool retro_media_admin_ = false;
    char retro_media_email_buffer_[192] = {};
    char retro_media_password_buffer_[192] = {};
    char retro_media_search_buffer_[128] = {};
    char retro_media_group_ = 0;
    struct CatalogueItem {
        std::string slug;
        std::string name;
        int rom_files = 0;
        long long total_bytes = 0;
    };
    std::vector<CatalogueItem> retro_media_catalogue_;

    std::vector<LibraryDisc> active_discs_;
    size_t active_disc_ = 0;

    char bios_path_buffer_[512] = {};
    char disc_path_buffer_[512] = {};
    std::string bios_target_;
    std::string disc_target_;
};

}  // namespace retro3do
