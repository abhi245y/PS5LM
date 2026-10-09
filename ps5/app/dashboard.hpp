// PS5LM's screen, three pages switched with L1/R1:
//   - Dashboard: a bento grid of live tiles in the language of
//     ps5-homebrew-ui's "Pulse Dashboard" design (its palette, grid, panels,
//     focus ring and spatial navigation). Cross on the Model tile opens the
//     model's details, Triangle the model library.
//   - Settings: a rail of categories and rows of toggles and steppers, after
//     the kit's "Control Room" design, in the Pulse palette.
//   - Logs: app.log and llama.log, scrollable.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings.hpp"
#include "stats.hpp"

#include "core/input.hpp"
#include "core/tween.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/motion.hpp"

#include <string>
#include <vector>

namespace ps5lm {

struct DashboardFrame {
    hui::gfx::BackdropSpec backdrop;
    hui::gfx::DrawList     scene;
    hui::gfx::DrawList     overlay;
    bool                   glass         = false;
    std::uint32_t          glass_texture = 0;
};

class Dashboard {
  public:
    static constexpr int kTiles = 8;
    static constexpr int kPages = 3;
    static constexpr int kLogsPage = 2;

    explicit Dashboard(const hui::ui::Fonts & fonts) : fonts_(fonts) { ring_.snap(ring_rect()); }

    void set_live(const Live & live) { live_ = live; }
    void open_library();  // over the dashboard, cursor on the loaded model
    void set_settings(const Settings & s) { settings_ = s; }
    const Settings & settings() const { return settings_; }
    void set_logs(std::vector<std::string> app, std::vector<std::string> llama);
    int  page() const { return page_; }
    void update(const hui::InputFrame & input, float dt, hui::ui::Feedback & feedback);
    void draw(DashboardFrame & frame) const;

    // What the user asked for, once: load or unload a model, or the settings
    // changed (read them with settings()).
    struct Request {
        enum Kind { none, load, unload, settings } kind = none;
        std::string path;
    };
    Request take_request() {
        Request r = request_;
        request_ = {};
        return r;
    }

  private:
    hui::gfx::Rect ring_rect() const;
    void draw_tile(hui::gfx::DrawList & list, int tile) const;
    void draw_content(hui::gfx::DrawList & list, int tile, const hui::gfx::Rect & r, float t) const;
    void draw_header(hui::gfx::DrawList & list) const;
    void draw_library(hui::gfx::DrawList & list, std::uint32_t glass) const;
    void draw_hints(DashboardFrame & frame) const;
    void draw_tabs(hui::gfx::DrawList & list) const;
    void draw_details(hui::gfx::DrawList & list, std::uint32_t glass) const;
    void draw_settings(hui::gfx::DrawList & list) const;
    void draw_logs(hui::gfx::DrawList & list) const;
    void update_dashboard(const hui::InputFrame & input, hui::ui::Feedback & feedback);
    void update_settings(const hui::InputFrame & input, hui::ui::Feedback & feedback);
    void update_logs(const hui::InputFrame & input, hui::ui::Feedback & feedback);
    const std::vector<std::string> & log_view() const { return log_llama_ ? llama_view_ : app_view_; }
    void refilter_logs();

    const hui::ui::Fonts & fonts_;
    Live        live_;
    Request     request_;

    float age_   = 0;  // seconds since start: the entrance
    float clock_ = 0;  // idle motion
    int   focus_ = 0;
    int   came_from_ = -1;
    hui::Direction came_by_ = hui::Direction::none;
    hui::ui::SpringRect   ring_;
    hui::tween::Spring    lift_[kTiles];
    hui::ui::Pulse        refusal_;
    float refusal_x_ = 0, refusal_y_ = 0;

    bool  library_open_ = false;
    int   library_cursor_ = 0;
    hui::tween::Spring library_;   // 0 closed, 1 open
    hui::tween::Spring cursor_y_;  // the highlight's slot
    int slot(int row) const;       // a model row's line in the list, after the section headers

    bool details_open_ = false;
    hui::tween::Spring details_;   // 0 closed, 1 open

    int   page_ = 0;
    int   page_from_ = 0;
    float page_age_ = 10;          // seconds since the page changed

    Settings settings_;
    int   setting_category_ = 0;
    int   setting_row_ = 0;
    bool  in_rows_ = false;        // the focus is on the rows, not the rail
    hui::ui::SpringRect setting_focus_;

    std::vector<std::string> app_log_, llama_log_;    // as read
    std::vector<std::string> app_view_, llama_view_;  // after the filter
    bool  log_llama_ = false;
    bool  log_errors_ = false;
    int   log_scroll_ = 0;         // lines up from the newest
    mutable char size_text_[64] = {};
};

}  // namespace ps5lm
