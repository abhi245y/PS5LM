// PS5LM's screen: a bento grid of live tiles in the language of
// ps5-homebrew-ui's "Pulse Dashboard" design (its palette, grid, panels,
// focus ring and spatial navigation), showing the model, its speed, the GPU,
// the CPU and the memory they share. Cross on the Model tile opens the model
// library; picking one asks the app to load it.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "stats.hpp"

#include "core/input.hpp"
#include "core/tween.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/motion.hpp"

#include <string>

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

    explicit Dashboard(const hui::ui::Fonts & fonts) : fonts_(fonts) { ring_.snap(ring_rect()); }

    void set_live(const Live & live) { live_ = live; }
    void update(const hui::InputFrame & input, float dt, hui::ui::Feedback & feedback);
    void draw(DashboardFrame & frame) const;

    // What the user asked for in the library, once: load a model, or import
    // one from a USB drive. Empty path: nothing.
    struct Request {
        enum Kind { none, load, unload, import } kind = none;
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
    hui::tween::Spring cursor_y_;  // the highlight's row
    mutable char size_text_[64] = {};
};

}  // namespace ps5lm
