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
const Color kBrand       = Color::rgb(0x8288f0);  // PS5LM's periwinkle

constexpr float kMuted = 0.62f;
constexpr float kFaint = 0.38f;

constexpr float kGridX = 96.0f, kGridY = 164.0f, kGridW = 1728.0f, kGridH = 800.0f;
constexpr float kGap = 20.0f, kRadius = 28.0f, kPad = 28.0f;
constexpr float kLabelY = 46.0f, kLabelSize = 17.0f, kLift = 0.02f;
constexpr float kTau = 6.2831853f;
constexpr const char * kDash = "\xE2\x80\x94";

enum Tile : int { kSpeed, kGpu, kCpu, kMemory, kVram, kSession, kContext, kModel };

struct TileSpec {
    const char *  label;
    int           column, row, columns, rows;
    std::uint32_t accent;
};

// In reading order: ties in the navigation go to the earlier tile.
constexpr TileSpec kSpec[Dashboard::kTiles] = {
    { "GENERATION", 0, 0, 6, 2, 0x4fe0ff },
    { "GPU", 6, 0, 3, 2, 0x4fe0ff },
    { "CPU", 9, 0, 3, 2, 0xb8f26b },
    { "MEMORY", 0, 2, 3, 2, 0xffc24a },
    { "GPU MEMORY", 3, 2, 6, 1, 0xff6f9c },
    { "SESSION", 3, 3, 3, 1, 0x4fe0ff },
    { "CONTEXT", 6, 3, 3, 1, 0xffc24a },
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

}  // namespace

// ---- update -------------------------------------------------------------------

Rect Dashboard::ring_rect() const {
    return scaled(tile_rect(focus_), 1.0f + kLift).inset(-5.0f);
}

void Dashboard::update(const hui::InputFrame & input, float dt, ui::Feedback & feedback) {
    age_ += dt;
    clock_ += dt;
    const int rows = (int) live_.models.size();

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
    } else {
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
        if (input.is_pressed(Action::confirm) && focus_ == kModel) {
            open_library();
            feedback.play(hui::audio::Cue::open);
        }
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
    cursor_y_.target = (float) slot(library_cursor_);
    cursor_y_.update(dt, 20.0f);
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
        case kGpu: {
            const float gw = number(list, f, serving, "%.0f", L.gpu_busy * 100.0f * up, r.x + kPad, r.y + 118, 60, kInk);
            ui::text(list, f.regular, "% busy computing", r.x + kPad + gw + 10, r.y + 118, 22, kInk.with_alpha(kMuted));
            ui::text(list, f.regular, "PlayStation 5 GPU, Vulkan (RADV)", r.x + kPad, r.y + 152, 20, kInk.with_alpha(kFaint));
            history(list, L.gpu_history, { r.x + kPad, r.y + 186, r.w - 2 * kPad, 90 }, 1.0f, kCyan, t);
            list.rounded_rect({ r.x + kPad, r.y + 300, r.w - 2 * kPad, 1 }, 0, kInk.with_alpha(0.1f));
            caps(list, f, "MODEL ON THE GPU", r.x + kPad, r.y + 338, 15, kInk.with_alpha(kMuted));
            std::snprintf(text, sizeof(text), "%s GiB", gib(L.model_gib + L.kv_gib).c_str());
            ui::text(list, f.mono, text, r.x + r.w - kPad, r.y + 340, 24, kInk, gfx::Align::right);
            break;
        }
        case kCpu: {
            const float cw = number(list, f, true, "%.0f", L.cpu_use * 100.0f * up, r.x + kPad, r.y + 118, 60, kInk);
            std::snprintf(text, sizeof(text), "%% of %d CPUs", L.cpus);
            ui::text(list, f.regular, text, r.x + kPad + cw + 10, r.y + 118, 22, kInk.with_alpha(kMuted));
            ui::text(list, f.regular, "Zen 2, the app's share", r.x + kPad, r.y + 152, 20, kInk.with_alpha(kFaint));
            history(list, L.cpu_history, { r.x + kPad, r.y + 186, r.w - 2 * kPad, 90 }, 1.0f, kLime, t);
            list.rounded_rect({ r.x + kPad, r.y + 300, r.w - 2 * kPad, 1 }, 0, kInk.with_alpha(0.1f));
            caps(list, f, "CPU HEAP", r.x + kPad, r.y + 338, 15, kInk.with_alpha(kMuted));
            std::snprintf(text, sizeof(text), "%s GiB", gib(L.heap_gib).c_str());
            ui::text(list, f.mono, text, r.x + r.w - kPad, r.y + 340, 24, kInk, gfx::Align::right);
            break;
        }
        case kMemory: {
            const bool  known = L.pool_gib > 0;
            const float used  = known ? (float) ((L.pool_gib - L.free_gib) / L.pool_gib) : 0.0f;
            const float cx = r.cx(), cy = r.y + 178;
            list.arc(cx, cy, 98, 16, 0.0f, kTau, kInk.with_alpha(0.08f));
            list.arc(cx, cy, 98, 16, 0.0f, kTau * used * up, kAmber);
            number(list, f, known, "%.0f%%", used * 100.0f * up, cx, cy + 12, 50, kInk, gfx::Align::center);
            ui::text(list, f.regular, "in use", cx, cy + 42, 20, kInk.with_alpha(kMuted), gfx::Align::center);
            std::snprintf(text, sizeof(text), "%s of %s GiB", gib(L.pool_gib - L.free_gib).c_str(), gib(L.pool_gib).c_str());
            ui::text(list, f.mono, known ? text : kDash, cx, r.y + 330, 24, kInk, gfx::Align::center);
            ui::text(list, f.regular, "GPU and CPU share this pool", cx, r.y + 360, 18, kInk.with_alpha(kFaint),
                     gfx::Align::center);
            break;
        }
        case kVram: {
            // Each part capped at what is in use, so a model still loading
            // never pushes the bar past the pool.
            const double used  = std::max(0.0, L.pool_gib - L.free_gib);
            const double model = std::min(L.model_gib, used);
            const double kv    = std::min(L.kv_gib, used - model);
            const double other = used - model - kv;
            const double parts[4] = { model, kv, other, L.free_gib };
            const char * names[4] = { "Model", "KV cache", "Other", "Free" };
            const Color  colors[4] = { kRose, Color::rgb(0xffa9c4), Color::rgb(0xb48cff), kInk.with_alpha(0.16f) };
            std::snprintf(text, sizeof(text), "of %s GiB", gib(L.pool_gib).c_str());
            const float w = f.regular.measure(text, 22);
            ui::text(list, f.regular, text, r.x + r.w - kPad, r.y + 50, 22, kInk.with_alpha(kMuted), gfx::Align::right);
            number(list, f, L.pool_gib > 0, "%.1f", (L.pool_gib - L.free_gib) * up, r.x + r.w - kPad - w - 12, r.y + 50,
                   34, kInk, gfx::Align::right);
            const Rect b{ r.x + kPad, r.y + 76, r.w - 2 * kPad, 28 };
            list.rounded_rect(b, 10, kInk.with_alpha(0.05f));
            float x = b.x;
            for (int k = 0; k < 4 && L.pool_gib > 0; ++k) {
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
        case kSession: {
            const int s = (int) L.uptime_s;
            std::snprintf(text, sizeof(text), "%02d:%02d:%02d", s / 3600 % 100, s / 60 % 60, s % 60);
            ui::text(list, f.mono, text, r.x + kPad, r.y + 104, 40, kInk);
            std::snprintf(text, sizeof(text), "%llu tokens out, %llu in, %llu replies", (unsigned long long) L.tokens,
                          (unsigned long long) L.prompt_tokens, (unsigned long long) L.requests);
            ui::text(list, f.regular, text, r.x + kPad, r.y + 150, 20, kInk.with_alpha(kMuted));
            break;
        }
        case kContext: {
            if (L.ctx == 0) {
                ui::text(list, f.regular, "No model loaded", r.x + kPad, r.y + 104, 26, kInk.with_alpha(kFaint));
                break;
            }
            const float share = L.ctx ? (float) L.ctx_used / (float) L.ctx : 0.0f;
            const float w = number(list, f, L.ctx > 0, "%.0f", (double) L.ctx_used, r.x + kPad, r.y + 104, 40, kInk);
            std::snprintf(text, sizeof(text), "of %uk tokens", L.ctx / 1024);
            ui::text(list, f.regular, text, r.x + kPad + w + 12, r.y + 104, 22, kInk.with_alpha(kMuted));
            bar(list, { r.x + kPad, r.y + 126, r.w - 2 * kPad, 10 }, share * up, kAmber);
            std::snprintf(text, sizeof(text), "%s cache, %s GiB", L.kv_type.empty() ? kDash : L.kv_type.c_str(),
                          gib(L.kv_gib).c_str());
            ui::text(list, f.regular, text, r.x + kPad, r.y + 164, 20, kInk.with_alpha(kMuted));
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

void Dashboard::draw_hints(DashboardFrame & frame) const {
    const ui::GlyphStyle style = ui::GlyphStyle::dark();
    if (library_.value > 0.5f) {
        const bool on  = library_cursor_ < (int) live_.models.size();
        const ModelRow * m = on ? &live_.models[(size_t) library_cursor_] : nullptr;
        const char * act = !m ? "Load" : m->current ? "Unload" : "Load";
        const ui::Hint hints[] = { { ui::Button::dpad, "Choose" }, { ui::Button::cross, act }, { ui::Button::circle, "Back" } };
        ui::draw_hints(frame.overlay, fonts_, style, hints, 3, kGridX + kGridW, true);
    } else {
        const ui::Hint hints[] = { { ui::Button::dpad, "Move" }, { ui::Button::cross, "Models" } };
        ui::draw_hints(frame.scene, fonts_, style, hints, focus_ == kModel ? 2 : 1, kGridX + kGridW, true);
    }
}

void Dashboard::draw(DashboardFrame & frame) const {
    frame.backdrop.mode = gfx::BackdropMode::dots;
    frame.backdrop.colors[0] = Color::rgb(0x05070f);
    frame.backdrop.colors[1] = Color::rgb(0x0b1122);
    frame.backdrop.colors[2] = gfx::mix(Color::rgb(0x2b3d66), kBrand, 0.35f);
    frame.backdrop.time = clock_;

    gfx::DrawList & scene = frame.scene;
    const float open = library_.value;
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
    draw_header(scene);
    if (open > 0.01f) {
        frame.glass = true;
        draw_library(frame.overlay, frame.glass_texture);
    }
    draw_hints(frame);
}

}  // namespace ps5lm
