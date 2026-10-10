// What the user can change on the Settings page, kept in
// /data/PS5LM/settings.json. Unknown or missing keys keep their defaults, so
// an old file still loads after a new setting is added.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace ps5lm {

struct Settings {
    bool        auto_load = false;  // load default_model at launch
    std::string default_model;      // a path from the library, or empty
    uint32_t    ctx_cap   = 65536;  // the planner's longest context
    std::string kv_type   = "auto"; // "auto", "f16", "q8_0" or "q4_0"
    bool        sounds    = true;
    bool        tools     = true;   // file tools in the browser chat, inside the scratch folder
    uint32_t    scratch_limit_gib = 2;  // warn when the scratch folder grows past this
    std::string download_dir = "/data/PS5LM/models";  // where the market saves models

    bool operator==(const Settings &) const = default;
};

Settings load_settings(const char * path);
bool     save_settings(const char * path, const Settings & s);

}  // namespace ps5lm
