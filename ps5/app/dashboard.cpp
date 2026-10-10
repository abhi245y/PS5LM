// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout, palette, panels, the focus ring and spatial navigation follow
// ps5-homebrew-ui's src/concepts/dashboard.cpp ("Pulse Dashboard",
// BlackBearReloaded, GPL-3.0-or-later); the tiles and their data are PS5LM's.
#include "dashboard.hpp"
#include "version.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <span>

namespace ps5lm {

using hui::Action;
using hui::Direction;
using hui::gfx::Color;
using hui::gfx::Rect;
namespace gfx   = hui::gfx;
namespace ui    = hui::ui;
namespace tween = hui::tween;

namespace {

// ---- the design language (Pulse Dashboard's) --------------------------------

const Color kInk         = Color::rgb(0xe9eefc);
const Color kClear       = Color::rgb(0x000000, 0.0f);
const Color kBlack       = Color::rgb(0x000000);
const Color kPanelTop    = Color::rgb(0x171f33);
const Color kPanelBottom = Color::rgb(0x0e1424);
const Color kCyan        = Color::rgb(0x4fe0ff);  // speed and the GPU
const Color kLime        = Color::rgb(0xb8f26b);  // the CPU and the model
const Color kAmber       = Color::rgb(0xffc24a);  // memory
const Color kRose        = Color::rgb(0xff6f9c);  // GPU memory
const Color kBrand       = Color::rgb(0x8288f0);  // PS5LM's periwinkle, and the context
const Color kHeat        = Color::rgb(0xff8a5c);  // temperatures

constexpr float kMuted = 0.62f;
constexpr float kFaint = 0.38f;

constexpr float kGridX = 96.0f, kGridY = 164.0f, kGridW = 1728.0f, kGridH = 800.0f;
constexpr float kGap = 20.0f, kRadius = 28.0f, kPad = 28.0f;
constexpr float kLabelY = 46.0f, kLabelSize = 17.0f, kLift = 0.02f;
constexpr float kTau = 6.2831853f;
constexpr const char * kDash = "\xE2\x80\x94";

enum Tile : int { kSpeed, kUsage, kPower, kMemory, kContext, kStorage, kThermals, kModel };

struct TileSpec {
    const char *  label;
    int           column, row, columns, rows;
    std::uint32_t accent;
};

// In reading order: ties in the navigation go to the earlier tile.
constexpr TileSpec kSpec[Dashboard::kTiles] = {
    { "GENERATION", 0, 0, 6, 2, 0x4fe0ff },
    { "USAGE", 6, 0, 3, 2, 0x4fe0ff },
    { "POWER", 9, 0, 3, 2, 0xffd166 },
    { "MEMORY", 0, 2, 6, 1, 0xffc24a },
    { "CONTEXT", 0, 3, 3, 1, 0x8288f0 },
    { "STORAGE", 3, 3, 3, 1, 0xff6f9c },
    { "THERMALS", 6, 2, 3, 2, 0xff8a5c },
    { "MODEL", 9, 2, 3, 2, 0xb8f26b },
};

Color accent_of(int tile) { return Color::rgb(kSpec[tile].accent); }

Rect tile_rect(int tile) {
    const TileSpec & s = kSpec[tile];
    const float column = (kGridW - 11.0f * kGap) / 12.0f;
    const float row    = (kGridH - 3.0f * kGap) / 4.0f;
    return { kGridX + s.column * (column + kGap), kGridY + s.row * (row + kGap),
             s.columns * (column + kGap) - kGap, s.rows * (row + kGap) - kGap };
}

Rect scaled(const Rect & r, float scale) {
    return { r.cx() - r.w * scale * 0.5f, r.cy() - r.h * scale * 0.5f, r.w * scale, r.h * scale };
}

// Pulse Dashboard's spatial navigation: the candidate beyond the leading
// edge with the least travel, a penalty for missing sideways, and a light
// pull toward the aligned one.
int spatial_next(std::span<const Rect> rects, int from, Direction direction) {
    if (direction == Direction::none) {
        return -1;
    }
    const bool  horizontal = direction == Direction::left || direction == Direction::right;
    const float sign = direction == Direction::right || direction == Direction::down ? 1.0f : -1.0f;
    auto main_low   = [=](const Rect & r) { return horizontal ? r.x : r.y; };
    auto main_size  = [=](const Rect & r) { return horizontal ? r.w : r.h; };
    auto cross_low  = [=](const Rect & r) { return horizontal ? r.y : r.x; };
    auto cross_size = [=](const Rect & r) { return horizontal ? r.h : r.w; };
    const Rect & o = rects[(size_t) from];
    const float leading = sign > 0 ? main_low(o) + main_size(o) : main_low(o);
    int   best = -1;
    float best_score = 0;
    for (int i = 0; i < (int) rects.size(); ++i) {
        if (i == from) {
            continue;
        }
        const Rect & c = rects[(size_t) i];
        if ((main_low(c) + main_size(c) * 0.5f - leading) * sign <= 0) {
            continue;
        }
        const float facing = sign > 0 ? main_low(c) : main_low(c) + main_size(c);
        const float along  = std::max(0.0f, (facing - leading) * sign);
        const float miss   = std::max({ 0.0f, cross_low(c) - (cross_low(o) + cross_size(o)),
                                        cross_low(o) - (cross_low(c) + cross_size(c)) });
        const float offset = std::fabs((cross_low(c) + cross_size(c) * 0.5f) - (cross_low(o) + cross_size(o) * 0.5f));
        const float score  = along + 2.0f * miss + 0.25f * offset;
        if (best < 0 || score < best_score - 0.01f) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

Direction opposite(Direction d) {
    switch (d) {
        case Direction::left: return Direction::right;
        case Direction::right: return Direction::left;
        case Direction::up: return Direction::down;
        case Direction::down: return Direction::up;
        default: return Direction::none;
    }
}

float rise(float time, int index = 0, float step = 0.05f, float duration = 0.7f) {
    return tween::cubic_out(tween::clamp01((time - step * (float) index) / duration));
}

const char * state_word(ServerState s) {
    switch (s) {
        case ServerState::loading: return "LOADING";
        case ServerState::ready: return "READY";
        case ServerState::generating: return "GENERATING";
        case ServerState::failed: return "FAILED";
        default: return "NO MODEL";
    }
}

Color state_color(ServerState s) {
    switch (s) {
        case ServerState::ready: return kLime;
        case ServerState::generating: return kCyan;
        case ServerState::failed: return kRose;
        default: return kAmber;
    }
}

constexpr float kRowH = 78.0f;
constexpr Rect  kLibrary{ 360.0f, 150.0f, 1200.0f, 800.0f };
constexpr const char * kPageNames[Dashboard::kPages] = { "Dashboard", "Settings", "Logs" };

// ---- Settings: categories of rows, after the kit's "Control Room" ---------------

enum class Setting : int { auto_load, default_model, ctx_cap, kv_type, sounds };

struct SettingRow {
    Setting      id;
    const char * label;
    const char * help;
};

struct SettingCategory {
    const char *     name;
    const char *     about;
    SettingRow       rows[2];
    int              count;
};

constexpr SettingCategory kCategories[] = {
    { "Startup", "What the app does when it opens.",
      { { Setting::auto_load, "Load a model at launch", "Off: the app opens the library and waits for a choice." },
        { Setting::default_model, "Model to load", "One of the models the library lists." } },
      2 },
    { "Model", "How the planner sizes a model. Applies the next time a model loads.",
      { { Setting::ctx_cap, "Longest context", "The planner picks the longest context up to this that fits." },
        { Setting::kv_type, "KV cache", "Auto picks the best that fits; a fixed type trades context for quality." } },
      2 },
    { "App", "The app itself.",
      { { Setting::sounds, "Sounds", "Interface sounds on the TV." } },
      1 },
};
constexpr int kCategoryCount = 3;

constexpr uint32_t     kCtxSteps[] = { 4096, 8192, 16384, 32768, 65536, 131072 };
constexpr const char * kKvSteps[] = { "auto", "f16", "q8_0", "q4_0" };

constexpr float kRailX = 96.0f, kRailY = 236.0f, kRailW = 372.0f, kRailH = 72.0f, kRailPitch = 84.0f;
constexpr Rect  kSettingsPanel{ 504.0f, 236.0f, 1320.0f, 692.0f };
constexpr float kSetRowsY = kSettingsPanel.y + 112.0f, kSetRowH = 80.0f, kSetRowPitch = 92.0f;

Rect rail_rect(int category) { return { kRailX, kRailY + category * kRailPitch, kRailW, kRailH }; }
Rect setting_rect(int row) {
    return { kSettingsPanel.x + 24, kSetRowsY + row * kSetRowPitch, kSettingsPanel.w - 48, kSetRowH };
}

// ---- Logs ---------------------------------------------------------------------------

constexpr Rect  kLogPanel{ 96.0f, 164.0f, 1728.0f, 800.0f };
constexpr float kLogLineH = 27.0f;
constexpr int   kLogLines = 25;

}  // namespace

// ---- update -------------------------------------------------------------------

Rect Dashboard::ring_rect() const {
    return scaled(tile_rect(focus_), 1.0f + kLift).inset(-5.0f);
}

void Dashboard::update(const hui::InputFrame & input, float dt, ui::Feedback & feedback) {
    age_ += dt;
    clock_ += dt;
    page_age_ += dt;

    // L1/R1 change the page, unless an overlay has the focus.
    const bool overlay = library_open_ || details_open_;
    const int  step = input.is_pressed(Action::page_next) ? 1 : input.is_pressed(Action::page_prev) ? -1 : 0;
    if (step != 0 && !overlay) {
        const int next = page_ + step;
        if (next >= 0 && next < kPages) {
            page_from_ = page_;
            page_ = next;
            page_age_ = 0;
            feedback.play(hui::audio::Cue::tab, 1.0f + 0.04f * (float) page_, 0.0f);
            if (page_ == kLogsPage) {
                log_scroll_ = 0;
            }
        } else {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
    } else if (page_ == 0 || overlay) {
        update_dashboard(input, feedback);
    } else if (page_ == 1) {
        update_settings(input, feedback);
    } else {
        update_logs(input, feedback);
    }

    for (int i = 0; i < kTiles; ++i) {
        lift_[i].target = i == focus_ ? 1.0f : 0.0f;
        lift_[i].update(dt, 16.0f);
    }
    ring_.target(ring_rect());
    ring_.update(dt, 20.0f);
    refusal_.update(dt, 9.0f);
    library_.target = library_open_ ? 1.0f : 0.0f;
    library_.update(dt, 13.0f);
    details_.target = details_open_ ? 1.0f : 0.0f;
    details_.update(dt, 13.0f);
    cursor_y_.target = (float) slot(library_cursor_);
    cursor_y_.update(dt, 20.0f);
    const Rect target = in_rows_ ? setting_rect(setting_row_) : rail_rect(setting_category_);
    if (age_ <= dt) {
        setting_focus_.snap(target);
    }
    setting_focus_.target(target);
    setting_focus_.update(dt, 20.0f);
}

void Dashboard::update_dashboard(const hui::InputFrame & input, ui::Feedback & feedback) {
    const int rows = (int) live_.models.size();
    if (details_open_) {
        if (input.is_pressed(Action::north) && detail_tile_ == kModel) {
            details_open_ = false;
            open_library();
            feedback.play(hui::audio::Cue::open);
        } else if (input.is_pressed(Action::back) || input.is_pressed(Action::confirm)) {
            details_open_ = false;
            feedback.play(hui::audio::Cue::back);
        }
        return;
    }
    if (library_open_) {
        if (input.nav == Direction::up || input.nav == Direction::down) {
            const int next = library_cursor_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < rows) {
                library_cursor_ = next;
                feedback.play(hui::audio::Cue::focus, 1.0f + 0.03f * (float) next);
            } else if (!input.nav_repeat) {
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
                refusal_.trigger();
            }
        }
        if (input.is_pressed(Action::confirm) && rows > 0) {
            const ModelRow & m = live_.models[(size_t) library_cursor_];
            if (m.current) {
                request_ = { Request::unload, m.file };
                library_open_ = false;
                feedback.play(hui::audio::Cue::back);
            } else if (m.fits) {
                request_ = { Request::load, m.file };
                library_open_ = false;
                feedback.play(hui::audio::Cue::complete);
            } else {
                feedback.play(hui::audio::Cue::error);
                refusal_.trigger();
            }
        }
        if (input.is_pressed(Action::back)) {
            library_open_ = false;
            feedback.play(hui::audio::Cue::back);
        }
        return;
    }
    if (input.nav != Direction::none) {
        std::array<Rect, kTiles> rects;
        for (int i = 0; i < kTiles; ++i) {
            rects[(size_t) i] = tile_rect(i);
        }
        int next = input.nav == opposite(came_by_) ? came_from_ : spatial_next(rects, focus_, input.nav);
        if (next < 0) {
            next = spatial_next(rects, focus_, input.nav);
        }
        if (next >= 0) {
            came_from_ = focus_;
            came_by_   = input.nav;
            focus_     = next;
            const Rect r = tile_rect(focus_);
            feedback.play(hui::audio::Cue::focus, 1.08f - 0.16f * (r.cy() - kGridY) / kGridH, ui::pan_for_x(r.cx()));
        } else if (!input.nav_repeat) {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
            feedback.rumble(0.25f, 0.05f);
            refusal_.trigger();
            refusal_x_ = input.nav == Direction::right ? 1.0f : input.nav == Direction::left ? -1.0f : 0.0f;
            refusal_y_ = input.nav == Direction::down ? 1.0f : input.nav == Direction::up ? -1.0f : 0.0f;
        }
    }
    // USAGE and POWER expand on Cross.
    if ((focus_ == kUsage || focus_ == kPower) && input.is_pressed(Action::confirm)) {
        details_open_ = true;
        detail_tile_ = focus_;
        feedback.play(hui::audio::Cue::open);
        return;
    }
    // The Model tile: Cross shows the loaded model's details (or the library
    // when nothing is loaded), Triangle the library.
    if (focus_ == kModel && input.is_pressed(Action::north)) {
        open_library();
        feedback.play(hui::audio::Cue::open);
    } else if (focus_ == kModel && input.is_pressed(Action::confirm)) {
        if (live_.model_label.empty()) {
            open_library();
        } else {
            details_open_ = true;
            detail_tile_ = kModel;
        }
        feedback.play(hui::audio::Cue::open);
    }
}

namespace {

// The value of a setting as the row shows it, and one step of it.
std::string setting_text(const Settings & s, Setting id, const std::vector<ModelRow> & models) {
    char t[32];
    switch (id) {
        case Setting::default_model: {
            if (s.default_model.empty()) {
                return "None";
            }
            for (const auto & m : models) {
                if (m.file == s.default_model) {
                    return m.label;
                }
            }
            const size_t slash = s.default_model.find_last_of('/');
            return s.default_model.substr(slash == std::string::npos ? 0 : slash + 1) + " (missing)";
        }
        case Setting::ctx_cap:
            std::snprintf(t, sizeof(t), "%uk tokens", s.ctx_cap / 1024);
            return t;
        case Setting::kv_type: return s.kv_type == "auto" ? "Auto" : s.kv_type;
        default: return "";
    }
}

bool is_toggle(Setting id) { return id == Setting::auto_load || id == Setting::sounds; }

// Moves a stepper one place; false at either end.
bool step_setting(Settings & s, Setting id, int dir, const std::vector<ModelRow> & models) {
    switch (id) {
        case Setting::default_model: {
            // "None", then each model that fits, in the library's order.
            std::vector<std::string> choices = { "" };
            for (const auto & m : models) {
                if (m.fits) {
                    choices.push_back(m.file);
                }
            }
            int at = 0;
            for (int i = 0; i < (int) choices.size(); ++i) {
                if (choices[(size_t) i] == s.default_model) {
                    at = i;
                }
            }
            const int next = at + dir;
            if (next < 0 || next >= (int) choices.size()) {
                return false;
            }
            s.default_model = choices[(size_t) next];
            return true;
        }
        case Setting::ctx_cap: {
            int at = 4;
            for (int i = 0; i < 6; ++i) {
                if (kCtxSteps[i] == s.ctx_cap) {
                    at = i;
                }
            }
            if (at + dir < 0 || at + dir >= 6) {
                return false;
            }
            s.ctx_cap = kCtxSteps[at + dir];
            return true;
        }
        case Setting::kv_type: {
            int at = 0;
            for (int i = 0; i < 4; ++i) {
                if (s.kv_type == kKvSteps[i]) {
                    at = i;
                }
            }
            if (at + dir < 0 || at + dir >= 4) {
                return false;
            }
            s.kv_type = kKvSteps[at + dir];
            return true;
        }
        case Setting::auto_load: s.auto_load = !s.auto_load; return true;
        case Setting::sounds: s.sounds = !s.sounds; return true;
    }
    return false;
}

}  // namespace

void Dashboard::update_settings(const hui::InputFrame & input, ui::Feedback & feedback) {
    const SettingCategory & c = kCategories[setting_category_];
    if (!in_rows_) {
        if (input.nav == Direction::up || input.nav == Direction::down) {
            const int next = setting_category_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < kCategoryCount) {
                setting_category_ = next;
                setting_row_ = 0;
                feedback.play(hui::audio::Cue::focus, 1.0f + 0.04f * (float) next);
            } else if (!input.nav_repeat) {
                feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
            }
        }
        if (input.nav == Direction::right || input.is_pressed(Action::confirm)) {
            in_rows_ = true;
            feedback.play(hui::audio::Cue::select);
        }
        return;
    }
    if (input.nav == Direction::up || input.nav == Direction::down) {
        const int next = setting_row_ + (input.nav == Direction::down ? 1 : -1);
        if (next >= 0 && next < c.count) {
            setting_row_ = next;
            feedback.play(hui::audio::Cue::focus, 1.0f + 0.04f * (float) next);
        } else if (!input.nav_repeat) {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
    }
    const Setting id = c.rows[setting_row_].id;
    int dir = 0;
    if (input.nav == Direction::left || input.nav == Direction::right) {
        dir = input.nav == Direction::right ? 1 : -1;
    }
    if (is_toggle(id) && input.is_pressed(Action::confirm)) {
        dir = 1;
    }
    if (dir != 0) {
        if (is_toggle(id) && (input.nav == Direction::left || input.nav == Direction::right)) {
            // Left turns a switch off, right on; the other way refuses.
            const bool on = id == Setting::auto_load ? settings_.auto_load : settings_.sounds;
            if (on == (dir > 0)) {
                if (!input.nav_repeat) {
                    feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
                }
                dir = 0;
            }
        }
        if (dir != 0 && step_setting(settings_, id, dir, live_.models)) {
            request_ = { Request::settings, "" };
            feedback.play(is_toggle(id) ? hui::audio::Cue::toggle : hui::audio::Cue::slider, 1.0f + 0.05f * (float) dir);
        } else if (dir != 0 && !input.nav_repeat) {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
    }
    if (input.is_pressed(Action::back)) {
        in_rows_ = false;
        feedback.play(hui::audio::Cue::back);
    }
}

namespace {

// 2 error, 1 warning, 0 neither. llama.cpp marks its lines " E " and " W ";
// the rest is matched by words, so "failures=0" is not an error.
int log_level(const std::string & line) {
    std::string low = line;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return (char) std::tolower(ch); });
    const auto has = [&](const char * w) { return low.find(w) != std::string::npos; };
    if (line.find(" E ") != std::string::npos || has("error") || has("failed") || has("exception") || has("fatal") ||
        has("abort")) {
        return 2;
    }
    if (line.find(" W ") != std::string::npos || has("warn")) {
        return 1;
    }
    return 0;
}

}  // namespace

void Dashboard::set_logs(std::vector<std::string> app, std::vector<std::string> llama) {
    app_log_ = std::move(app);
    llama_log_ = std::move(llama);
    refilter_logs();
}

void Dashboard::refilter_logs() {
    const auto keep = [&](const std::string & line) { return !log_errors_ || log_level(line) > 0; };
    app_view_.clear();
    llama_view_.clear();
    std::copy_if(app_log_.begin(), app_log_.end(), std::back_inserter(app_view_), keep);
    std::copy_if(llama_log_.begin(), llama_log_.end(), std::back_inserter(llama_view_), keep);
    log_scroll_ = std::min(log_scroll_, std::max(0, (int) log_view().size() - kLogLines));
}

void Dashboard::update_logs(const hui::InputFrame & input, ui::Feedback & feedback) {
    const int most = std::max(0, (int) log_view().size() - kLogLines);
    if (input.nav == Direction::up || input.nav == Direction::down) {
        const int next = std::clamp(log_scroll_ + (input.nav == Direction::up ? 3 : -3), 0, most);
        if (next != log_scroll_) {
            log_scroll_ = next;
            feedback.play(hui::audio::Cue::focus, 1.0f, 0.0f, 0.4f);
        } else if (!input.nav_repeat) {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
    }
    if (input.is_pressed(Action::west)) {
        log_llama_ = !log_llama_;
        log_scroll_ = 0;
        feedback.play(hui::audio::Cue::select);
    }
    if (input.is_pressed(Action::north)) {
        log_errors_ = !log_errors_;
        refilter_logs();
        log_scroll_ = 0;
        feedback.play(hui::audio::Cue::select);
    }
    if (input.is_pressed(Action::confirm)) {
        log_scroll_ = 0;  // back to the newest
        feedback.play(hui::audio::Cue::back);
    }
}

// ---- drawing helpers -----------------------------------------------------------

namespace {

float caps(gfx::DrawList & list, const ui::Fonts & f, std::string_view v, float x, float y, float size, Color c,
           gfx::Align a = gfx::Align::left) {
    return ui::text(list, f.semibold, v, x, y, size, c, a, 3.0f);
}

// A value, or a dash while there is nothing measured.
float number(gfx::DrawList & list, const ui::Fonts & f, bool known, const char * format, double v, float x, float y,
             float size, Color c, gfx::Align a = gfx::Align::left) {
    char text[32];
    if (known) {
        std::snprintf(text, sizeof(text), format, v);
    } else {
        std::snprintf(text, sizeof(text), "%s", kDash);
    }
    return ui::text(list, f.mono, text, x, y, size, known ? c : c.with_alpha(kFaint), a);
}

// A filled history chart, newest on the right, scaled to `top`.
void history(gfx::DrawList & list, const std::vector<float> & v, const Rect & g, float top, Color c, float t) {
    const float base = g.y + g.h;
    list.rounded_rect({ g.x, base - 1.0f, g.w, 2.0f }, 1, kInk.with_alpha(0.22f));
    if (v.size() < 2) {
        list.rounded_rect({ g.x, base - 1.5f, g.w, 3.0f }, 1.5f, c.with_alpha(0.45f));
        return;
    }
    const float grow = rise(t, 0, 0.0f, 0.9f);
    const float step = g.w / 119.0f;
    float lx = 0, ly = 0;
    for (size_t age = 0; age < v.size(); ++age) {
        const float value = v[v.size() - 1 - age];
        const float x = g.x + g.w - (float) age * step;
        const float y = base - tween::clamp01(value / top) * g.h * grow;
        list.gradient_rect({ x - step * 0.5f, y, step, base - y }, 0, c.with_alpha(0.5f * (base - y) / g.h),
                           c.with_alpha(0.0f));
        if (age > 0) {
            list.line(lx, ly, x, y, 2.5f, c);
        }
        lx = x;
        ly = y;
    }
    const float newest = base - tween::clamp01(v.back() / top) * g.h * grow;
    list.circle(g.x + g.w, newest, 6, c);
    list.circle(g.x + g.w, newest, 3, kPanelBottom);
}

void bar(gfx::DrawList & list, const Rect & r, float share, Color c) {
    list.rounded_rect(r, r.h * 0.5f, kInk.with_alpha(0.1f));
    list.rounded_rect({ r.x, r.y, std::max(r.h, r.w * tween::clamp01(share)), r.h }, r.h * 0.5f, c);
}

void draw_panel(gfx::DrawList & list, const Rect & r, float radius, float lit, Color accent) {
    list.gradient_rect(r, radius, gfx::mix(kPanelTop, accent, 0.05f * lit).with_alpha(0.9f),
                       gfx::mix(kPanelBottom, accent, 0.02f * lit).with_alpha(0.92f));
    list.bordered_rect(r, radius, kClear, 1.5f, gfx::mix(kInk.with_alpha(0.09f), accent.with_alpha(0.5f), lit));
}

std::string gib(double v) {
    char t[24];
    std::snprintf(t, sizeof(t), "%.1f", v);
    return t;
}

}  // namespace

// ---- tiles ------------------------------------------------------------------------

void Dashboard::draw_content(gfx::DrawList & list, int tile, const Rect & r, float t) const {
    const ui::Fonts & f = fonts_;
    const Live & L = live_;
    const bool serving = L.state == ServerState::ready || L.state == ServerState::generating;
    const float up = rise(t, 0, 0.0f, 0.9f);
    char text[96];

    switch (tile) {
        case kSpeed: {
            float peak = 1, sum = 0, mn = 1e9f;
            int   n = 0;
            for (float v : L.gen_history) {
                if (v > 0) {
                    peak = std::max(peak, v);
                    mn = std::min(mn, v);
                    sum += v;
                    n++;
                }
            }
            // Idle, the big figure keeps the last generation's speed.
            float shown = L.gen_tps;
            for (auto it = L.gen_history.rbegin(); shown <= 0 && it != L.gen_history.rend(); ++it) {
                shown = *it;
            }
            caps(list, f, "TOK/S", r.x + r.w - kPad, r.y + 84, 17, kInk.with_alpha(kMuted), gfx::Align::right);
            number(list, f, serving && shown > 0, "%.1f", shown * up, r.x + r.w - kPad - 76, r.y + 84, 64,
                   L.state == ServerState::generating ? kInk : kInk.with_alpha(0.8f), gfx::Align::right);
            if (L.state == ServerState::loading) {
                const float p = std::max(0.0f, L.load_progress);
                std::snprintf(text, sizeof(text), "Loading the model onto the GPU  \xC2\xB7  %.0f%%", p * 100.0f);
                const Rect lb{ r.x + kPad, r.y + 196, r.w - 2 * kPad, 14 };
                list.rounded_rect(lb, 7, kInk.with_alpha(0.08f));
                // A bright sheen runs along the filled part while it grows.
                const float w = std::max(14.0f, lb.w * p);
                list.rounded_rect({ lb.x, lb.y, w, lb.h }, 7, kCyan);
                const float sheen = std::fmod(clock_ * 0.6f, 1.0f);
                list.glow({ lb.x + w * sheen - 20, lb.y, 40, lb.h }, 7, 16, kInk.with_alpha(0.35f));
                std::snprintf(size_text_, sizeof(size_text_), "%.1f of %.1f GiB on the GPU", p * (L.model_gib + L.kv_gib),
                              L.model_gib + L.kv_gib);
                ui::text(list, f.regular, size_text_, r.x + kPad, r.y + 250, 20, kInk.with_alpha(kFaint));
            } else if (!serving) {
                std::snprintf(text, sizeof(text), "%s", "Not serving");
            } else if (L.state == ServerState::generating) {
                std::snprintf(text, sizeof(text), "Generating now");
            } else if (n == 0) {
                std::snprintf(text, sizeof(text), "Waiting for the first prompt");
            } else {
                std::snprintf(text, sizeof(text), "Idle, %llu tokens so far", (unsigned long long) L.tokens);
            }
            ui::text(list, f.regular, text, r.x + kPad, r.y + 84, 22, kInk.with_alpha(kMuted));
            if (L.state != ServerState::loading) {
                history(list, L.gen_history, { r.x + kPad, r.y + 116, r.w - 2 * kPad, 186 }, peak * 1.2f, kCyan, t);
            }
            const char * names[] = { "MIN", "AVG", "MAX" };
            const float  vals[] = { n ? mn : 0, n ? sum / n : 0, n ? peak : 0 };
            for (int k = 0; k < 3; ++k) {
                const float x = r.x + kPad + k * 170.0f;
                caps(list, f, names[k], x, r.y + 352, 15, kInk.with_alpha(kMuted));
                number(list, f, n > 0, "%.1f", vals[k], x + 56, r.y + 354, 26, kInk);
            }
            const float uw = ui::text(list, f.regular, "tok/s", r.x + r.w - kPad, r.y + 353, 20, kInk.with_alpha(kFaint),
                                      gfx::Align::right);
            const float pw = number(list, f, L.prompt_tps > 0, "%.0f", L.prompt_tps, r.x + r.w - kPad - uw - 10, r.y + 354, 26,
                                    kInk, gfx::Align::right);
            caps(list, f, "PROMPT", r.x + r.w - kPad - uw - pw - 26, r.y + 352, 15, kInk.with_alpha(kMuted), gfx::Align::right);
            break;
        }
        case kUsage: {
            // The GPU and the CPU side by side over one chart: cyan the GPU,
            // lime the app's CPUs.
            const float half = (r.w - 2 * kPad) * 0.5f;
            caps(list, f, "GPU", r.x + kPad, r.y + 86, 15, kCyan);
            caps(list, f, "CPU", r.x + kPad + half, r.y + 86, 15, kLime);
            float w = number(list, f, serving, "%.0f", L.gpu_busy * 100.0f * up, r.x + kPad, r.y + 140, 52, kInk);
            ui::text(list, f.regular, "%", r.x + kPad + w + 6, r.y + 140, 22, kInk.with_alpha(kMuted));
            w = number(list, f, true, "%.0f", L.cpu_use * 100.0f * up, r.x + kPad + half, r.y + 140, 52, kInk);
            ui::text(list, f.regular, "%", r.x + kPad + half + w + 6, r.y + 140, 22, kInk.with_alpha(kMuted));
            const Rect g{ r.x + kPad, r.y + 170, r.w - 2 * kPad, 110 };
            history(list, L.cpu_history, g, 1.0f, kLime, t);
            history(list, L.gpu_history, g, 1.0f, kCyan, t);
            list.rounded_rect({ r.x + kPad, r.y + 300, r.w - 2 * kPad, 1 }, 0, kInk.with_alpha(0.1f));
            caps(list, f, "CLOCK", r.x + kPad, r.y + 338, 15, kInk.with_alpha(kMuted));
            std::snprintf(text, sizeof(text), "%d CPUs, %.1f GHz", L.cpus, L.cpu_ghz);
            ui::text(list, f.mono, L.cpu_ghz > 0 ? text : kDash, r.x + r.w - kPad, r.y + 340, 22, kInk, gfx::Align::right);
            break;
        }
        case kPower: {
            const bool known = L.soc_power_w > 0;
            const float pw = number(list, f, known, "%.0f", L.soc_power_w * up, r.x + kPad, r.y + 118, 60, kInk);
            ui::text(list, f.regular, "W, the SoC", r.x + kPad + pw + 10, r.y + 118, 22, kInk.with_alpha(kMuted));
            if (known && L.state == ServerState::generating && L.gen_tps > 0) {
                std::snprintf(text, sizeof(text), "%.1f J per token", L.soc_power_w / L.gen_tps);
            } else if (known) {
                std::snprintf(text, sizeof(text), "CPU, GPU and memory together");
            } else {
                std::snprintf(text, sizeof(text), "Load ps5-exporter to read it");
            }
            ui::text(list, f.regular, text, r.x + kPad, r.y + 152, 20, kInk.with_alpha(kFaint));
            float peak = 60;
            for (float v : L.power_history) {
                peak = std::max(peak, v);
            }
            history(list, L.power_history, { r.x + kPad, r.y + 186, r.w - 2 * kPad, 90 }, peak * 1.15f,
                    Color::rgb(0xffd166), t);
            list.rounded_rect({ r.x + kPad, r.y + 300, r.w - 2 * kPad, 1 }, 0, kInk.with_alpha(0.1f));
            caps(list, f, "THIS SESSION", r.x + kPad, r.y + 338, 15, kInk.with_alpha(kMuted));
            number(list, f, L.energy_wh > 0, "%.2f Wh", L.energy_wh, r.x + r.w - kPad, r.y + 340, 22, kInk, gfx::Align::right);
            break;
        }
        case kMemory: {
            // One pool: the GPU and the CPU share the title's direct memory.
            // Each part is capped at what is in use, so a model still loading
            // never pushes the bar past the pool.
            const bool   known = L.pool_gib > 0;
            const double used  = std::max(0.0, L.pool_gib - L.free_gib);
            const double model = std::min(L.model_gib, used);
            const double kv    = std::min(L.kv_gib, used - model);
            const double other = used - model - kv;
            const double parts[4] = { model, kv, other, L.free_gib };
            const char * names[4] = { "Model", "KV cache", "Other", "Free" };
            const Color  colors[4] = { kAmber, Color::rgb(0xffe2a0), Color::rgb(0xb48cff), kInk.with_alpha(0.16f) };
            std::snprintf(text, sizeof(text), "of %s GiB, GPU and CPU", gib(L.pool_gib).c_str());
            const float w = f.regular.measure(text, 22);
            ui::text(list, f.regular, text, r.x + r.w - kPad, r.y + 50, 22, kInk.with_alpha(kMuted), gfx::Align::right);
            const float uw = number(list, f, known, "%.1f", used * up, r.x + r.w - kPad - w - 12, r.y + 50, 34, kInk,
                                    gfx::Align::right);
            number(list, f, known, "%.0f%%", (known ? used / L.pool_gib * 100.0 : 0.0) * up,
                   r.x + r.w - kPad - w - uw - 34, r.y + 50, 34, kAmber, gfx::Align::right);
            const Rect b{ r.x + kPad, r.y + 76, r.w - 2 * kPad, 28 };
            list.rounded_rect(b, 10, kInk.with_alpha(0.05f));
            float x = b.x;
            for (int k = 0; k < 4 && known; ++k) {
                const float full = b.w * (float) (parts[k] / L.pool_gib);
                const float seg  = std::max(0.0f, full * rise(t, k, 0.16f, 0.5f) - 4.0f);
                if (seg > 1.0f) {
                    list.rounded_rect({ x, b.y, seg, b.h }, 10, colors[k]);
                }
                x += full * rise(t, k, 0.16f, 0.5f);
            }
            x = r.x + kPad;
            for (int k = 0; k < 4; ++k) {
                list.circle(x + 7, r.y + 143, 7, k == 3 ? kInk.with_alpha(0.45f) : colors[k]);
                x += 24;
                x += ui::text(list, f.regular, names[k], x, r.y + 150, 20, kInk.with_alpha(0.85f));
                x += 10 + ui::text(list, f.mono, gib(parts[k]), x + 10, r.y + 150, 20, kInk.with_alpha(kMuted));
                x += 36;
            }
            break;
        }
        case kContext: {
            const int s = (int) L.uptime_s;
            if (L.ctx == 0) {
                ui::text(list, f.regular, "No model loaded", r.x + kPad, r.y + 104, 26, kInk.with_alpha(kFaint));
            } else {
                const float share = (float) L.ctx_used / (float) L.ctx;
                const float w = number(list, f, true, "%.0f", (double) L.ctx_used, r.x + kPad, r.y + 104, 40, kInk);
                std::snprintf(text, sizeof(text), "of %uk tokens", L.ctx / 1024);
                ui::text(list, f.regular, text, r.x + kPad + w + 12, r.y + 104, 22, kInk.with_alpha(kMuted));
                bar(list, { r.x + kPad, r.y + 126, r.w - 2 * kPad, 10 }, share * up, kBrand);
            }
            char up_text[16];
            std::snprintf(up_text, sizeof(up_text), "%02d:%02d:%02d", s / 3600 % 100, s / 60 % 60, s % 60);
            ui::text(list, f.mono, up_text, r.x + r.w - kPad, r.y + kLabelY, 18, kInk.with_alpha(kMuted), gfx::Align::right);
            std::snprintf(text, sizeof(text), "%llu out  \xC2\xB7  %llu in  \xC2\xB7  %llu replies",
                          (unsigned long long) L.tokens, (unsigned long long) L.prompt_tokens,
                          (unsigned long long) L.requests);
            ui::text(list, f.regular, f.regular.font->fit(text, 18, r.w - 2 * kPad), r.x + kPad, r.y + 164, 18,
                     kInk.with_alpha(kMuted));
            break;
        }
        case kStorage: {
            const struct {
                const char * name;
                double       free, total;
            } drives[2] = { { "INTERNAL", L.data_free, L.data_total }, { "USB", L.usb_free, L.usb_total } };
            for (int k = 0; k < 2; ++k) {
                const float y = r.y + 92 + k * 52.0f;
                caps(list, f, drives[k].name, r.x + kPad, y, 15, kInk.with_alpha(kMuted));
                if (drives[k].total <= 0) {
                    ui::text(list, f.regular, k == 1 ? "No drive" : kDash, r.x + r.w - kPad, y, 20, kInk.with_alpha(kFaint),
                             gfx::Align::right);
                    continue;
                }
                std::snprintf(text, sizeof(text), "%.0f GiB free", drives[k].free);
                ui::text(list, f.mono, text, r.x + r.w - kPad, y, 20, kInk, gfx::Align::right);
                bar(list, { r.x + kPad, y + 14, r.w - 2 * kPad, 8 },
                    (float) ((drives[k].total - drives[k].free) / drives[k].total) * up, kRose);
            }
            break;
        }
        case kThermals: {
            const bool known = L.soc_temp > -100;
            const float tw = number(list, f, known, "%.0f", L.soc_temp, r.x + kPad, r.y + 118, 60, kInk);
            ui::text(list, f.regular, "\xC2\xB0" "C, the SoC", r.x + kPad + tw + 10, r.y + 118, 22, kInk.with_alpha(kMuted));
            if (L.cpu_temp > -100 && L.fan >= 0) {
                std::snprintf(text, sizeof(text), "CPU %.0f \xC2\xB0" "C  \xC2\xB7  fan %.0f%%", L.cpu_temp, L.fan * 100.0f);
            } else if (L.cpu_temp > -100) {
                std::snprintf(text, sizeof(text), "CPU %.0f \xC2\xB0" "C", L.cpu_temp);
            } else if (!L.sensors_from_exporter) {
                std::snprintf(text, sizeof(text), "Load ps5-exporter to read them");
            } else {
                std::snprintf(text, sizeof(text), "No sensor answered");
            }
            ui::text(list, f.regular, text, r.x + kPad, r.y + 152, 20, kInk.with_alpha(kFaint));
            history(list, L.temp_history, { r.x + kPad, r.y + 186, r.w - 2 * kPad, 90 }, 100.0f, kHeat, t);
            list.rounded_rect({ r.x + kPad, r.y + 300, r.w - 2 * kPad, 1 }, 0, kInk.with_alpha(0.1f));
            // The hottest sensor, so a hot spot shows without opening POWER.
            const std::pair<std::string, float> * hot = nullptr;
            for (const auto & tp : L.temps) {
                if (!hot || tp.second > hot->second) {
                    hot = &tp;
                }
            }
            caps(list, f, "HOTTEST", r.x + kPad, r.y + 338, 15, kInk.with_alpha(kMuted));
            if (hot) {
                const std::string who = hot->first == "cpu" ? std::string("CPU") : "SoC " + hot->first.substr(3);
                std::snprintf(text, sizeof(text), "%s, %.0f \xC2\xB0" "C", who.c_str(), hot->second);
            }
            ui::text(list, f.mono, hot ? text : kDash, r.x + r.w - kPad, r.y + 340, 22, kInk, gfx::Align::right);
            break;
        }
        default: {  // kModel
            const float width = r.w - 2 * kPad;
            if (L.model_label.empty()) {
                ui::text(list, f.semibold, "No model loaded", r.x + kPad, r.y + 104, 34, kInk);
                ui::text(list, f.regular, "Cross opens the library", r.x + kPad, r.y + 138, 20, kInk.with_alpha(kMuted));
                std::snprintf(text, sizeof(text), "%zu found", L.models.size());
                ui::text(list, f.regular, text, r.x + kPad, r.y + 352, 20, kInk.with_alpha(kFaint));
                break;
            }
            const std::string name = L.model_label.empty() ? std::string("No model") : L.model_label;
            ui::text(list, f.semibold, f.semibold.font->fit(L.preset.empty() ? name : L.preset, 34, width), r.x + kPad,
                     r.y + 104, 34, kInk);
            ui::text(list, f.regular, f.regular.font->fit(name, 20, width), r.x + kPad, r.y + 138, 20,
                     kInk.with_alpha(kMuted));
            const std::string rows[3][2] = {
                { "SIZE", gib(L.model_gib) + " GiB" },
                { "CONTEXT", L.ctx ? std::to_string(L.ctx / 1024) + "k, " + L.kv_type : std::string(kDash) },
                { "ARCH", L.model_arch.empty() ? std::string(kDash) : L.model_arch },
            };
            for (int k = 0; k < 3; ++k) {
                const float y = r.y + 196 + k * 46.0f;
                caps(list, f, rows[k][0], r.x + kPad, y, 15, kInk.with_alpha(kMuted));
                ui::text(list, f.mono, rows[k][1], r.x + r.w - kPad, y + 2, 22, kInk, gfx::Align::right);
            }
            std::snprintf(text, sizeof(text), "%zu found", L.models.size());
            ui::text(list, f.regular, text, r.x + kPad, r.y + 352, 20, kInk.with_alpha(kFaint));
            break;
        }
    }
}

void Dashboard::draw_tile(gfx::DrawList & list, int tile) const {
    const float in = tween::stagger(age_, tile, 0.06f, 0.5f);
    const float lift = lift_[tile].value;
    const Color accent = accent_of(tile);
    Rect r = tile_rect(tile);
    r.y += 28.0f * (1.0f - in);
    float dx = 0, dy = 0;
    if (tile == focus_ && !library_open_) {
        const float nudge = ui::shake(refusal_.value, clock_, 10.0f, 9.0f);
        dx = nudge * refusal_x_;
        dy = nudge * refusal_y_;
    }
    list.push_opacity(in);
    list.push_transform(1.0f + kLift * lift, r.cx(), r.cy(), dx, dy);
    if (lift > 0.01f) {
        list.shadow({ r.x, r.y + 16, r.w, r.h }, kRadius, 38, kBlack.with_alpha(0.5f * lift));
        list.glow(r, kRadius, 26, accent.with_alpha((0.16f + 0.1f * ui::breathe(clock_)) * lift));
    }
    draw_panel(list, r, kRadius, lift, accent);
    caps(list, fonts_, kSpec[tile].label, r.x + kPad, r.y + kLabelY, kLabelSize, accent);
    draw_content(list, tile, r, age_ - 0.25f - 0.06f * tile);
    list.pop_transform();
    list.pop_opacity();
}

void Dashboard::draw_header(gfx::DrawList & list) const {
    const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
    list.push_opacity(in);
    const float y = 124 - 10.0f * (1.0f - in);
    const float w = ui::text(list, fonts_.display, "PS5LM", kGridX - 3, y, 56, kInk);
    std::string sub = "v" PS5LM_VERSION "  \xC2\xB7  Chat at " + live_.address;
    ui::text(list, fonts_.regular, sub, kGridX + w + 24, y, 24, kInk.with_alpha(kMuted));

    const char * word  = state_word(live_.state);
    const Color  dot   = state_color(live_.state);
    const float  chip_w = 70.0f + fonts_.semibold.measure(word, 17) + 3.0f * (float) std::char_traits<char>::length(word);
    const Rect   chip{ kGridX + kGridW - chip_w, 82, chip_w, 48 };
    list.bordered_rect(chip, 24, kPanelTop.with_alpha(0.9f), 1.5f, dot.with_alpha(0.4f));
    const float beat = live_.state == ServerState::loading || live_.state == ServerState::generating ? ui::breathe(clock_, 1.2f) : 0.6f;
    list.glow({ chip.x + 20, chip.cy() - 7, 14, 14 }, 7, 12, dot.with_alpha(0.7f * beat));
    list.circle(chip.x + 27, chip.cy(), 7, dot);
    caps(list, fonts_, word, chip.x + 48, chip.cy() + 6, 17, kInk);
    list.pop_opacity();
}

void Dashboard::draw_library(gfx::DrawList & list, std::uint32_t glass) const {
    const float t = library_.value;
    if (t <= 0.01f) {
        return;
    }
    const ui::Fonts & f = fonts_;
    Rect panel = kLibrary;
    panel.y += 30.0f * (1.0f - t);
    list.push_opacity(t);
    list.shadow({ panel.x, panel.y + 20, panel.w, panel.h }, 40, 50, kBlack.with_alpha(0.55f));
    if (glass) {
        list.glass(glass, panel, 40, Color::rgb(0xffffff));
    }
    draw_panel(list, panel, 40, 1.0f, kLime);
    caps(list, f, "MODEL LIBRARY", panel.x + 56, panel.y + 60, 20, kLime);
    ui::text(list, f.regular,
             f.regular.font->fit("Settings fit the memory free now. Models go in /data/PS5LM/models, or PS5LM/models on USB.",
                                 20, panel.w - 112),
             panel.x + 56, panel.y + 98, 20, kInk.with_alpha(kMuted));

    const float top = panel.y + 132;
    const int   rows = (int) live_.models.size();
    if (rows == 0) {
        ui::text(list, f.regular, "No models found yet", panel.cx(), panel.cy(), 26, kInk.with_alpha(kFaint), gfx::Align::center);
    }
    // Eight lines show, section headers included; the list scrolls to keep
    // the cursor among them.
    const int   first = rows > 0 ? std::max(0, slot(library_cursor_) - 7) : 0;
    const Rect hl{ panel.x + 36, top + (cursor_y_.value - (float) first) * kRowH, panel.w - 72, kRowH - 8 };
    if (rows > 0) {
        list.glow(hl, 18, 18, kLime.with_alpha(0.25f));
        list.bordered_rect(hl, 18, kLime.with_alpha(0.1f), 2.0f, kLime.with_alpha(0.7f));
    }
    for (int i = 0; i < rows; ++i) {
        const ModelRow & m = live_.models[(size_t) i];
        const int   s = slot(i);
        if (i == 0 || m.on_usb != live_.models[(size_t) i - 1].on_usb) {
            const int h = s - 1;
            if (h >= first && h < first + 8) {
                const float hy = top + (float) (h - first) * kRowH;
                caps(list, f, m.on_usb ? "USB DRIVE" : "INTERNAL STORAGE", panel.x + 60, hy + 50, 16,
                     m.on_usb ? kAmber : kCyan);
                list.rounded_rect({ panel.x + 60, hy + 62, panel.w - 120, 1 }, 0, kInk.with_alpha(0.12f));
            }
        }
        if (s < first || s >= first + 8) {
            continue;
        }
        const float y = top + (float) (s - first) * kRowH;
        const float ink = m.fits ? 1.0f : 0.45f;
        ui::text(list, f.semibold, f.semibold.font->fit(m.label, 24, 620), panel.x + 60, y + 32, 24, kInk.with_alpha(ink));
        std::string sub = m.plan;
        if (!m.preset.empty()) {
            sub = m.preset + " preset  \xC2\xB7  " + sub;
        }
        ui::text(list, f.regular, f.regular.font->fit(sub, 18, 700), panel.x + 60, y + 58, 18, kInk.with_alpha(kMuted * ink));
        char size[24];
        std::snprintf(size, sizeof(size), "%.1f GiB", m.size_gib);
        ui::text(list, f.mono, size, panel.x + panel.w - 260, y + 44, 22, kInk.with_alpha(ink), gfx::Align::right);
        const char * tag = m.current ? "LOADED" : m.fits ? "LOAD" : "TOO BIG";
        const Color  tc  = m.current ? kCyan : m.fits ? kLime : kRose;
        caps(list, f, tag, panel.x + panel.w - 60, y + 44, 15, tc, gfx::Align::right);
    }
    list.pop_opacity();
}

void Dashboard::open_library() {
    library_open_ = true;
    library_cursor_ = 0;
    for (int i = 0; i < (int) live_.models.size(); ++i) {
        if (live_.models[(size_t) i].current) {
            library_cursor_ = i;
        }
    }
    cursor_y_.snap((float) slot(library_cursor_));
}

// Rows come sorted by path, so internal (/data) ones precede USB (/mnt) ones;
// each group gets a header line above it.
int Dashboard::slot(int row) const {
    int s = row + 1;
    if (row > 0 && live_.models[(size_t) row].on_usb && !live_.models[0].on_usb) {
        ++s;
    }
    return s;
}

void Dashboard::draw_tabs(gfx::DrawList & list) const {
    // Left of the state chip: L1, the three page names, R1. The active name
    // is lit and underlined; the underline glides between names.
    const ui::Fonts & f = fonts_;
    const char * word = state_word(live_.state);
    const float chip_w = 70.0f + f.semibold.measure(word, 17) + 3.0f * (float) std::char_traits<char>::length(word);
    float x = kGridX + kGridW - chip_w - 40;
    const ui::GlyphStyle style = ui::GlyphStyle::dark();
    const float in = tween::stagger(age_, 1, 0.05f, 0.5f);
    list.push_opacity(in);
    x -= ui::button_width(ui::Button::r1, 26);
    ui::draw_button(list, f, style, ui::Button::r1, x, 106, 26);
    std::array<float, kPages> xs{}, ws{};
    for (int i = kPages - 1; i >= 0; --i) {
        ws[(size_t) i] = f.semibold.measure(kPageNames[i], 22);
        x -= 28 + ws[(size_t) i];
        xs[(size_t) i] = x;
        ui::text(list, i == page_ ? f.semibold : f.regular, kPageNames[i], x, 114, 22,
                 kInk.with_alpha(i == page_ ? 1.0f : kMuted));
    }
    x -= 16 + ui::button_width(ui::Button::l1, 26);
    ui::draw_button(list, f, style, ui::Button::l1, x, 106, 26);
    const float t = tween::cubic_out(tween::clamp01(page_age_ / 0.3f));
    const float ux = xs[(size_t) page_from_] + (xs[(size_t) page_] - xs[(size_t) page_from_]) * t;
    const float uw = ws[(size_t) page_from_] + (ws[(size_t) page_] - ws[(size_t) page_from_]) * t;
    list.rounded_rect({ ux, 128, uw, 3 }, 1.5f, kCyan);
    list.pop_opacity();
}

namespace {

// llama-server's arguments as flag and value pairs, one per line.
std::vector<std::string> arg_lines(const std::vector<std::string> & args) {
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string line = args[i];
        const bool flag = line.size() > 1 && line[0] == '-' && !std::isdigit((unsigned char) line[1]);
        if (flag && i + 1 < args.size()) {
            const std::string & v = args[i + 1];
            if (!(v.size() > 1 && v[0] == '-' && !std::isdigit((unsigned char) v[1]))) {
                line += " " + v;
                ++i;
            }
        }
        out.push_back(line);
    }
    return out;
}

}  // namespace

void Dashboard::draw_details(gfx::DrawList & list, std::uint32_t glass) const {
    const float t = details_.value;
    if (t <= 0.01f) {
        return;
    }
    const ui::Fonts & f = fonts_;
    const Live & L = live_;
    Rect panel = kLibrary;
    panel.y += 30.0f * (1.0f - t);
    list.push_opacity(t);
    list.shadow({ panel.x, panel.y + 20, panel.w, panel.h }, 40, 50, kBlack.with_alpha(0.55f));
    if (glass) {
        list.glass(glass, panel, 40, Color::rgb(0xffffff));
    }
    if (detail_tile_ == kUsage || detail_tile_ == kPower) {
        draw_panel(list, panel, 40, 1.0f, accent_of(detail_tile_));
        if (detail_tile_ == kUsage) {
            draw_usage_details(list, panel);
        } else {
            draw_power_details(list, panel);
        }
        list.pop_opacity();
        return;
    }
    draw_panel(list, panel, 40, 1.0f, kLime);
    caps(list, f, "LOADED MODEL", panel.x + 56, panel.y + 60, 20, kLime);
    ui::text(list, f.semibold, f.semibold.font->fit(L.preset.empty() ? L.model_label : L.preset, 40, panel.w - 112),
             panel.x + 56, panel.y + 116, 40, kInk);
    ui::text(list, f.regular, f.regular.font->fit(L.model_file, 20, panel.w - 112), panel.x + 56, panel.y + 150, 20,
             kInk.with_alpha(kMuted));

    // Left: the facts. Right: every argument llama-server got.
    char text[96];
    std::snprintf(text, sizeof(text), "%s GiB", gib(L.model_gib).c_str());
    std::string ctx = L.ctx ? std::to_string(L.ctx / 1024) + "k tokens" : std::string(kDash);
    std::string cache = L.kv_type.empty() ? std::string(kDash) : L.kv_type + ", " + gib(L.kv_gib) + " GiB";
    const std::string facts[][2] = {
        { "SIZE", text },
        { "ARCHITECTURE", L.model_arch.empty() ? std::string(kDash) : L.model_arch },
        { "CONTEXT", ctx },
        { "KV CACHE", cache },
        { "PRESET", L.preset.empty() ? std::string("None") : L.preset },
    };
    const float lx = panel.x + 56, lw = 400;
    for (int k = 0; k < 5; ++k) {
        const float y = panel.y + 220 + k * 62.0f;
        caps(list, f, facts[k][0], lx, y, 15, kInk.with_alpha(kMuted));
        ui::text(list, f.mono, f.mono.font->fit(facts[k][1], 24, lw), lx, y + 32, 24, kInk);
    }
    caps(list, f, "PLAN", lx, panel.y + 548, 15, kInk.with_alpha(kMuted));
    ui::paragraph(list, f.regular, L.plan.empty() ? std::string("Set by app-args.txt") : L.plan, lx, panel.y + 580, 20,
                  lw, 28, kInk.with_alpha(0.85f), 3);

    const float ax = panel.x + 520, aw = panel.w - 520 - 56;
    list.rounded_rect({ ax - 32, panel.y + 196, 1, panel.h - 260 }, 0, kInk.with_alpha(0.1f));
    caps(list, f, "ARGUMENTS", ax, panel.y + 220, 15, kInk.with_alpha(kMuted));
    const auto lines = arg_lines(L.args);
    const int  per_column = 17;
    const float col_w = lines.size() > (size_t) per_column ? aw * 0.5f : aw;
    for (size_t i = 0; i < lines.size() && i < (size_t) per_column * 2; ++i) {
        const float x = ax + (float) (i / per_column) * col_w;
        const float y = panel.y + 256 + (float) (i % per_column) * 30.0f;
        ui::text(list, f.mono, f.mono.font->fit(lines[i], 18, col_w - 16), x, y, 18, kInk.with_alpha(0.9f));
    }
    list.pop_opacity();
}

// Everything the console is doing, one section each.
void Dashboard::draw_usage_details(gfx::DrawList & list, const Rect & panel) const {
    const ui::Fonts & f = fonts_;
    const Live & L = live_;
    const bool serving = L.state == ServerState::ready || L.state == ServerState::generating;
    char text[96];
    caps(list, f, "USAGE", panel.x + 56, panel.y + 60, 20, kCyan);
    ui::text(list, f.regular, "What the console is doing for PS5LM, over the last two minutes.", panel.x + 56,
             panel.y + 96, 20, kInk.with_alpha(kMuted));

    const float x0 = panel.x + 56, colw = (panel.w - 112 - 48) * 0.5f, x1 = x0 + colw + 48;
    // GPU and CPU, each with its chart.
    const struct {
        const char * name;
        Color        c;
        float        now;
        bool         known;
        const std::vector<float> * hist;
        std::string  sub;
        float        x;
    } parts[2] = {
        { "GPU", kCyan, L.gpu_busy, serving, &L.gpu_history,
          "Vulkan (RADV), " + gib(L.model_gib + L.kv_gib) + " GiB on it", x0 },
        { "CPU", kLime, L.cpu_use, true, &L.cpu_history,
          std::to_string(L.cpus) + " CPUs" + (L.cpu_ghz > 0 ? ", " + gib(L.cpu_ghz) + " GHz" : std::string()) +
              ", heap " + gib(L.heap_gib) + " GiB",
          x1 },
    };
    for (const auto & p : parts) {
        caps(list, f, p.name, p.x, panel.y + 160, 16, p.c);
        const float w = number(list, f, p.known, "%.0f", p.now * 100.0f, p.x, panel.y + 222, 56, kInk);
        ui::text(list, f.regular, "% busy", p.x + w + 10, panel.y + 222, 22, kInk.with_alpha(kMuted));
        ui::text(list, f.regular, f.regular.font->fit(p.sub, 20, colw), p.x, panel.y + 256, 20, kInk.with_alpha(kFaint));
        history(list, *p.hist, { p.x, panel.y + 280, colw, 150 }, 1.0f, p.c, 10.0f);
    }

    // Memory and storage.
    list.rounded_rect({ x0, panel.y + 470, panel.w - 112, 1 }, 0, kInk.with_alpha(0.1f));
    caps(list, f, "MEMORY", x0, panel.y + 514, 16, kAmber);
    const double used = std::max(0.0, L.pool_gib - L.free_gib);
    std::snprintf(text, sizeof(text), "%s of %s GiB, GPU and CPU", gib(used).c_str(), gib(L.pool_gib).c_str());
    ui::text(list, f.mono, text, x0 + colw, panel.y + 514, 20, kInk, gfx::Align::right);
    bar(list, { x0, panel.y + 534, colw, 12 }, L.pool_gib > 0 ? (float) (used / L.pool_gib) : 0.0f, kAmber);

    caps(list, f, "STORAGE", x1, panel.y + 514, 16, kRose);
    const struct {
        const char * name;
        double       free, total;
    } drives[2] = { { "Internal", L.data_free, L.data_total }, { "USB", L.usb_free, L.usb_total } };
    for (int k = 0; k < 2; ++k) {
        const float y = panel.y + 570 + k * 70.0f;
        ui::text(list, f.regular, drives[k].name, x1, y, 20, kInk.with_alpha(0.85f));
        if (drives[k].total <= 0) {
            ui::text(list, f.regular, "No drive", x1 + colw, y, 20, kInk.with_alpha(kFaint), gfx::Align::right);
            continue;
        }
        std::snprintf(text, sizeof(text), "%.0f of %.0f GiB free", drives[k].free, drives[k].total);
        ui::text(list, f.mono, text, x1 + colw, y, 20, kInk, gfx::Align::right);
        bar(list, { x1, y + 16, colw, 10 }, (float) ((drives[k].total - drives[k].free) / drives[k].total), kRose);
    }
    caps(list, f, "MODELS", x0, panel.y + 610, 16, kInk.with_alpha(kMuted));
    double on_disk = 0;
    for (const auto & m : L.models) {
        on_disk += m.size_gib;
    }
    std::snprintf(text, sizeof(text), "%zu found, %s GiB", L.models.size(), gib(on_disk).c_str());
    ui::text(list, f.mono, text, x0 + colw, panel.y + 610, 20, kInk, gfx::Align::right);
}

// The SoC's power, the energy it took, and every temperature it reports.
void Dashboard::draw_power_details(gfx::DrawList & list, const Rect & panel) const {
    const ui::Fonts & f = fonts_;
    const Live & L = live_;
    const Color kGold = Color::rgb(0xffd166);
    char text[96];
    caps(list, f, "POWER", panel.x + 56, panel.y + 60, 20, kGold);
    ui::text(list, f.regular, "The SoC's own reading: CPU, GPU and memory together.", panel.x + 56, panel.y + 96, 20,
             kInk.with_alpha(kMuted));

    const float x0 = panel.x + 56, lw = 640, x1 = x0 + lw + 64, rw = panel.x + panel.w - 56 - x1;
    const float w = number(list, f, L.soc_power_w > 0, "%.0f", L.soc_power_w, x0, panel.y + 200, 72, kInk);
    ui::text(list, f.regular, "W now", x0 + w + 12, panel.y + 200, 24, kInk.with_alpha(kMuted));
    float mn = 1e9f, mx = 0, sum = 0;
    for (float v : L.power_history) {
        mn = std::min(mn, v);
        mx = std::max(mx, v);
        sum += v;
    }
    const size_t n = L.power_history.size();
    history(list, L.power_history, { x0, panel.y + 236, lw, 200 }, std::max(60.0f, mx * 1.15f), kGold, 10.0f);
    const char * names[3] = { "MIN", "AVG", "MAX" };
    const float  vals[3] = { n ? mn : 0, n ? sum / (float) n : 0, mx };
    for (int k = 0; k < 3; ++k) {
        const float x = x0 + k * 190.0f;
        caps(list, f, names[k], x, panel.y + 486, 15, kInk.with_alpha(kMuted));
        number(list, f, n > 0, "%.0f W", vals[k], x + 56, panel.y + 488, 24, kInk);
    }
    const struct {
        const char * name;
        std::string  value;
    } rows[3] = {
        { "THIS SESSION", [&] { char t[24]; std::snprintf(t, sizeof(t), "%.2f Wh", L.energy_wh); return std::string(t); }() },
        { "PER TOKEN", L.gen_tps > 0 && L.soc_power_w > 0 ? gib(L.soc_power_w / L.gen_tps) + " J while generating"
                                                          : std::string(kDash) },
        { "FAN", L.fan >= 0 ? std::to_string((int) (L.fan * 100.0f + 0.5f)) + "% duty" : std::string(kDash) },
    };
    for (int k = 0; k < 3; ++k) {
        const float y = panel.y + 556 + k * 50.0f;
        caps(list, f, rows[k].name, x0, y, 15, kInk.with_alpha(kMuted));
        ui::text(list, f.mono, rows[k].value, x0 + lw, y + 2, 22, kInk, gfx::Align::right);
    }

    // Every sensor, hottest first by bar length.
    list.rounded_rect({ x1 - 32, panel.y + 150, 1, panel.h - 220 }, 0, kInk.with_alpha(0.1f));
    caps(list, f, "TEMPERATURES", x1, panel.y + 160, 15, kHeat);
    if (L.temps.empty()) {
        ui::text(list, f.regular, "No sensor answered", x1, panel.y + 200, 20, kInk.with_alpha(kFaint));
    }
    for (size_t k = 0; k < L.temps.size() && k < 12; ++k) {
        const float y = panel.y + 204 + (float) k * 40.0f;
        std::string name = L.temps[k].first;
        if (name == "cpu") {
            name = "CPU";
        } else {
            name = "SoC " + name.substr(3);
        }
        ui::text(list, f.regular, name, x1, y, 20, kInk.with_alpha(0.85f));
        std::snprintf(text, sizeof(text), "%.0f \xC2\xB0" "C", L.temps[k].second);
        ui::text(list, f.mono, text, x1 + rw, y, 20, kInk, gfx::Align::right);
        bar(list, { x1 + 90, y - 8, rw - 180, 6 }, L.temps[k].second / 100.0f, kHeat);
    }
    ui::paragraph(list, f.regular,
                  "The console reports the SoC's total only. CPU and GPU power apart, and voltages, are not available to "
                  "homebrew.",
                  x0, panel.y + panel.h - 52, 17, panel.w - 112, 24, kInk.with_alpha(kFaint), 2);
}

void Dashboard::draw_settings(gfx::DrawList & list) const {
    const ui::Fonts & f = fonts_;
    const SettingCategory & c = kCategories[setting_category_];
    const float in = tween::cubic_out(tween::clamp01(page_age_ / 0.35f));

    // The rail: a quiet plate marks the active category even while the focus
    // is on the rows.
    list.rounded_rect(rail_rect(setting_category_), 18, kInk.with_alpha(0.06f));
    draw_panel(list, kSettingsPanel, kRadius, 0.0f, kBrand);

    // The one focus rectangle, travelling between the rail and the rows.
    const Rect focus = setting_focus_.value();
    const float breath = ui::breathe(clock_);
    list.shadow({ focus.x, focus.y + 10, focus.w, focus.h }, 18, 24, kBlack.with_alpha(0.4f));
    list.glow(focus, 18, 18, kCyan.with_alpha(0.14f + 0.1f * breath));
    list.rounded_rect(focus, 18, gfx::mix(kPanelTop, kCyan, 0.17f));
    list.bordered_rect(focus, 18, kClear, 2.0f, kCyan);

    for (int i = 0; i < kCategoryCount; ++i) {
        const Rect r = rail_rect(i);
        const bool active = i == setting_category_;
        const float x = r.x - 28.0f * (1.0f - tween::stagger(page_age_, i, 0.05f, 0.4f));
        ui::text(list, active ? f.semibold : f.regular, kCategories[i].name, x + 40, r.cy() + 10, 28,
                 kInk.with_alpha(active ? 1.0f : 0.68f));
    }
    list.rounded_rect({ kRailX + 8, rail_rect(setting_category_).y + 20, 4, kRailH - 40 }, 2, kCyan);

    caps(list, f, ui::upper(c.name), kSettingsPanel.x + 48, kSettingsPanel.y + 56, 18, kBrand);
    ui::text(list, f.regular, c.about, kSettingsPanel.x + 48, kSettingsPanel.y + 90, 22, kInk.with_alpha(kMuted));
    for (int i = 0; i < c.count; ++i) {
        const Rect r = setting_rect(i);
        const bool focused = in_rows_ && i == setting_row_;
        const float y = r.y + 16.0f * (1.0f - tween::stagger(page_age_, i + 1, 0.06f, 0.4f));
        const Rect row{ r.x, y, r.w, r.h };
        if (!focused) {
            list.rounded_rect(row, 18, kInk.with_alpha(0.035f));
        }
        const SettingRow & sr = c.rows[i];
        ui::text(list, focused ? f.semibold : f.regular, sr.label, row.x + 28, row.cy() + 10, 28,
                 kInk.with_alpha(focused ? 1.0f : 0.84f));
        const float right = row.x + row.w - 28;
        if (is_toggle(sr.id)) {
            const bool on = sr.id == Setting::auto_load ? settings_.auto_load : settings_.sounds;
            const Rect pill{ right - 84, row.cy() - 22, 84, 44 };
            if (focused) {
                list.glow(pill, 22, 12, kCyan.with_alpha(on ? 0.45f : 0.18f));
            }
            list.bordered_rect(pill, 22, on ? kCyan : Color::rgb(0x2a3550), 1.5f, on ? kCyan : kInk.with_alpha(0.3f));
            const Rect thumb{ on ? pill.x + pill.w - 39 : pill.x + 5, pill.cy() - 17, 34, 34 };
            list.shadow({ thumb.x, thumb.y + 2, thumb.w, thumb.h }, 17, 8, kBlack.with_alpha(0.45f));
            list.rounded_rect(thumb, 17, on ? kPanelBottom : kInk);
            ui::text(list, f.regular, on ? "On" : "Off", pill.x - 20, row.cy() + 8, 24,
                     kInk.with_alpha(focused ? 0.95f : 0.66f), gfx::Align::right);
        } else {
            // A stepper: the value between two triangles, dimmed at the ends.
            const float w = 420, cx = right - w * 0.5f;
            if (focused) {
                list.rounded_rect({ cx - w * 0.5f + 34, row.cy() - 24, w - 68, 48 }, 14, kBlack.with_alpha(0.25f));
            }
            const std::string v = setting_text(settings_, sr.id, live_.models);
            ui::text(list, f.semibold, f.semibold.font->fit(v, 26, w - 100), cx, row.cy() + 9, 26,
                     focused ? kInk : kInk.with_alpha(0.84f), gfx::Align::center);
            Settings probe = settings_;
            const bool left_ok = step_setting(probe, sr.id, -1, live_.models);
            probe = settings_;
            const bool right_ok = step_setting(probe, sr.id, 1, live_.models);
            const Color tint = focused ? kCyan : kInk;
            const float rest = focused ? 1.0f : 0.5f;
            ui::text(list, f.mono, "\xE2\x97\x80", cx - w * 0.5f + 12, row.cy() + 10, 30,
                     tint.with_alpha(left_ok ? rest : 0.16f), gfx::Align::center);
            ui::text(list, f.mono, "\xE2\x96\xB6", cx + w * 0.5f - 12, row.cy() + 10, 30,
                     tint.with_alpha(right_ok ? rest : 0.16f), gfx::Align::center);
        }
    }
    // The focused row's help under a hairline at the foot of the panel.
    const float foot = kSettingsPanel.y + kSettingsPanel.h - 84;
    list.rounded_rect({ kSettingsPanel.x + 24, foot, kSettingsPanel.w - 48, 1.5f }, 0, kInk.with_alpha(0.1f));
    const char * help = in_rows_ ? c.rows[setting_row_].help : "Changes are saved at once, to /data/PS5LM/settings.json.";
    ui::text(list, f.regular, help, kSettingsPanel.x + 48, foot + 50, 22, kInk.with_alpha(kMuted * in));
}

void Dashboard::draw_logs(gfx::DrawList & list) const {
    const ui::Fonts & f = fonts_;
    const Rect & p = kLogPanel;
    draw_panel(list, p, kRadius, 0.0f, kCyan);
    // Two sources as tabs, the filter as a chip, and the position.
    float x = p.x + kPad;
    const char * names[2] = { "APP.LOG", "LLAMA.LOG" };
    for (int k = 0; k < 2; ++k) {
        const bool on = (k == 1) == log_llama_;
        const float w = caps(list, f, names[k], x, p.y + kLabelY, kLabelSize, on ? kCyan : kInk.with_alpha(kFaint));
        if (on) {
            list.rounded_rect({ x, p.y + kLabelY + 12, w, 3 }, 1.5f, kCyan);
        }
        x += w + 36;
    }
    if (log_errors_) {
        const Rect chip{ x, p.y + 22, 170, 34 };
        list.bordered_rect(chip, 17, kRose.with_alpha(0.12f), 1.5f, kRose.with_alpha(0.6f));
        caps(list, f, "ERRORS ONLY", chip.cx(), chip.cy() + 6, 14, kRose, gfx::Align::center);
    }
    const auto & lines = log_view();
    char text[64];
    if (log_scroll_ > 0) {
        std::snprintf(text, sizeof(text), "%zu %s, %d up from the newest", lines.size(), lines.size() == 1 ? "line" : "lines", log_scroll_);
    } else {
        std::snprintf(text, sizeof(text), "%zu %s, following the newest", lines.size(), lines.size() == 1 ? "line" : "lines");
    }
    ui::text(list, f.regular, text, p.x + p.w - kPad, p.y + kLabelY, 20, kInk.with_alpha(kMuted), gfx::Align::right);
    list.rounded_rect({ p.x + kPad, p.y + 72, p.w - 2 * kPad, 1 }, 0, kInk.with_alpha(0.1f));

    if (lines.empty()) {
        ui::text(list, f.regular, log_errors_ ? "No errors or warnings" : "Nothing logged yet", p.cx(), p.cy(), 26,
                 kInk.with_alpha(kFaint), gfx::Align::center);
        return;
    }
    const int last  = (int) lines.size() - 1 - log_scroll_;
    const int first = std::max(0, last - kLogLines + 1);
    for (int i = first; i <= last; ++i) {
        const std::string & line = lines[(size_t) i];
        const int level = log_level(line);
        const Color c = level == 2 ? kRose : level == 1 ? kAmber : line.rfind("ps5lm-app:", 0) == 0 ? kInk : kInk.with_alpha(kMuted);
        const float y = p.y + 108 + (float) (i - first) * kLogLineH;
        ui::text(list, f.mono, f.mono.font->fit(line, 17, p.w - 2 * kPad - 24), p.x + kPad, y, 17, c);
    }
    // Where the view is in the log.
    const float track_h = kLogLines * kLogLineH;
    const float shown = std::min(1.0f, (float) kLogLines / (float) lines.size());
    const float top = 1.0f - (float) (last + 1) / (float) lines.size();
    list.rounded_rect({ p.x + p.w - 18, p.y + 88, 4, track_h }, 2, kInk.with_alpha(0.08f));
    list.rounded_rect({ p.x + p.w - 18, p.y + 88 + track_h * top, 4, std::max(24.0f, track_h * shown) }, 2,
                      kCyan.with_alpha(0.8f));
}

void Dashboard::draw_hints(DashboardFrame & frame) const {
    const ui::GlyphStyle style = ui::GlyphStyle::dark();
    if (library_.value > 0.5f) {
        const bool on  = library_cursor_ < (int) live_.models.size();
        const ModelRow * m = on ? &live_.models[(size_t) library_cursor_] : nullptr;
        const char * act = !m ? "Load" : m->current ? "Unload" : "Load";
        const ui::Hint hints[] = { { ui::Button::dpad, "Choose" }, { ui::Button::cross, act }, { ui::Button::circle, "Back" } };
        ui::draw_hints(frame.overlay, fonts_, style, hints, 3, kGridX + kGridW, true);
    } else if (details_.value > 0.5f) {
        const ui::Hint hints[] = { { ui::Button::triangle, "Library" }, { ui::Button::circle, "Back" } };
        const bool model = detail_tile_ == kModel;
        ui::draw_hints(frame.overlay, fonts_, style, model ? hints : hints + 1, model ? 2 : 1, kGridX + kGridW, true);
    } else if (page_ == 1) {
        const bool toggle = in_rows_ && is_toggle(kCategories[setting_category_].rows[setting_row_].id);
        if (!in_rows_) {
            const ui::Hint hints[] = { { ui::Button::dpad, "Choose" }, { ui::Button::cross, "Open" } };
            ui::draw_hints(frame.scene, fonts_, style, hints, 2, kGridX + kGridW, true);
        } else {
            const ui::Hint hints[] = { { ui::Button::dpad, "Change" },
                                       { ui::Button::cross, toggle ? "Switch" : "" },
                                       { ui::Button::circle, "Back" } };
            const ui::Hint plain[] = { { ui::Button::dpad, "Change" }, { ui::Button::circle, "Back" } };
            ui::draw_hints(frame.scene, fonts_, style, toggle ? hints : plain, toggle ? 3 : 2, kGridX + kGridW, true);
        }
    } else if (page_ == kLogsPage) {
        const ui::Hint hints[] = { { ui::Button::dpad, "Scroll" },
                                   { ui::Button::square, log_llama_ ? "app.log" : "llama.log" },
                                   { ui::Button::triangle, log_errors_ ? "All lines" : "Errors only" },
                                   { ui::Button::cross, "Newest" } };
        ui::draw_hints(frame.scene, fonts_, style, hints, 4, kGridX + kGridW, true);
    } else if (focus_ == kModel) {
        const bool loaded = !live_.model_label.empty();
        const ui::Hint hints[] = { { ui::Button::dpad, "Move" },
                                   { ui::Button::cross, loaded ? "Details" : "Library" },
                                   { ui::Button::triangle, "Library" } };
        ui::draw_hints(frame.scene, fonts_, style, hints, loaded ? 3 : 2, kGridX + kGridW, true);
    } else {
        const ui::Hint hints[] = { { ui::Button::dpad, "Move" }, { ui::Button::cross, "Expand" } };
        const bool expands = focus_ == kUsage || focus_ == kPower;
        ui::draw_hints(frame.scene, fonts_, style, hints, expands ? 2 : 1, kGridX + kGridW, true);
    }
}

void Dashboard::draw(DashboardFrame & frame) const {
    frame.backdrop.mode = gfx::BackdropMode::dots;
    frame.backdrop.colors[0] = Color::rgb(0x05070f);
    frame.backdrop.colors[1] = Color::rgb(0x0b1122);
    frame.backdrop.colors[2] = gfx::mix(Color::rgb(0x2b3d66), kBrand, 0.35f);
    frame.backdrop.time = clock_;

    gfx::DrawList & scene = frame.scene;
    // A page arrives from the side it was asked for and fades in.
    const float in = tween::cubic_out(tween::clamp01(page_age_ / 0.35f));
    const float from = page_ > page_from_ ? 1.0f : -1.0f;
    scene.push_transform(1.0f, 960, 564, 60.0f * from * (1.0f - in), 0);
    scene.push_opacity(in);
    if (page_ == 0) {
        const float open = std::max(library_.value, details_.value);
        scene.push_transform(1.0f - 0.04f * open, 960, 564, 0, 0);
        scene.push_opacity(1.0f - 0.6f * open);
        for (int i = 0; i < kTiles; ++i) {
            if (i != focus_) {
                draw_tile(scene, i);
            }
        }
        draw_tile(scene, focus_);  // last: its glow and shadow sit on its neighbours
        const float alpha = tween::stagger(age_, focus_, 0.06f, 0.5f) * (1.0f - tween::clamp01(open * 4.0f));
        if (alpha > 0.01f) {
            Rect ring = ring_.value();
            const float nudge = ui::shake(refusal_.value, clock_, 10.0f, 9.0f);
            ring.x += nudge * refusal_x_;
            ring.y += nudge * refusal_y_;
            scene.bordered_rect(ring, kRadius + 5.0f, kClear, 3.0f, accent_of(focus_).with_alpha(alpha));
        }
        scene.pop_opacity();
        scene.pop_transform();
    } else if (page_ == 1) {
        draw_settings(scene);
    } else {
        draw_logs(scene);
    }
    scene.pop_opacity();
    scene.pop_transform();
    draw_header(scene);
    draw_tabs(scene);
    if (library_.value > 0.01f || details_.value > 0.01f) {
        frame.glass = true;
        draw_library(frame.overlay, frame.glass_texture);
        draw_details(frame.overlay, frame.glass_texture);
    }
    draw_hints(frame);
}

}  // namespace ps5lm
