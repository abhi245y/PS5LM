// Model plans for the PS5LM app: what is in a GGUF, and the llama-server
// arguments that fit it on the console's GPU.
//
// A plan picks the longest context and the best KV cache type that fit the
// device memory left after the weights, from the model's own metadata (only
// attention layers keep a KV cache; hybrid models such as Qwen 3.5/3.8 keep
// one in four, and their recurrent layers a fixed state). Known models add
// their recommended sampling from the preset table.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5lm {

struct ModelInfo {
    std::string path;
    std::string file_name;
    std::string name;   // general.name, or the file name
    std::string arch;   // general.architecture
    uint64_t    file_bytes      = 0;
    uint32_t    n_layer         = 0;
    uint32_t    n_head_kv       = 0;
    uint32_t    key_length      = 0;   // per KV head
    uint32_t    value_length    = 0;
    uint32_t    full_attn_every = 1;   // 1: every layer is attention
    uint32_t    n_ctx_train     = 0;
    uint32_t    n_expert        = 0;   // 0: dense
    uint64_t    state_bytes     = 0;   // recurrent state, all layers (f32)
    bool        ok              = false;
    std::string error;

    uint32_t n_kv_layers() const { return full_attn_every > 1 ? n_layer / full_attn_every : n_layer; }
};

// Reads the metadata of a GGUF file (not its tensors).
ModelInfo read_model_info(const std::string & path);

// Bytes of KV cache per token of context, for a cache type ("f16", "q8_0", "q4_0").
double kv_bytes_per_token(const ModelInfo & m, const std::string & type);

struct Preset {
    const char * match;      // case-insensitive substring of the file or model name
    const char * label;      // what the UI calls it
    const char * note;       // one line about it on a PS5
    std::vector<std::string> args;   // sampling and template arguments
    uint32_t     max_ctx = 0;        // 0: the planner decides
};

const std::vector<Preset> & presets();
const Preset * find_preset(const ModelInfo & m);

struct Plan {
    bool        fits = false;
    uint32_t    ctx  = 0;
    std::string kv_type;      // cache type for K and V
    double      weights_gib  = 0;
    double      kv_gib       = 0;
    double      state_gib    = 0;
    double      reserve_gib  = 0;   // compute buffers and the CPU side
    double      total_gib    = 0;
    std::string why;          // one line for the UI
    std::vector<std::string> args;  // full llama-server arguments
};

// budget_gib: device memory free before loading (the app reads it from ggml).
// kv_force: one cache type ("f16", "q8_0", "q4_0") instead of the best that fits.
Plan plan_model(const ModelInfo & m, double budget_gib, uint32_t ctx_cap = 65536, const char * kv_force = nullptr);

// Every .gguf in the given folders (not recursive), sorted by name. Split
// files are listed by their first part only.
std::vector<std::string> find_models(const std::vector<std::string> & dirs);

}  // namespace ps5lm
