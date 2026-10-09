// SPDX-License-Identifier: GPL-3.0-or-later
#include "model_plan.hpp"

#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

namespace ps5lm {

namespace {

constexpr double kGiB = 1073741824.0;

// GGUF metadata value types.
enum : uint32_t { U8, I8, U16, I16, U32, I32, F32, BOOL, STRING, ARRAY, U64, I64, F64 };

struct Reader {
    FILE * f;
    bool   bad = false;

    template <typename T> T get() {
        T v{};
        if (std::fread(&v, sizeof(v), 1, f) != 1) {
            bad = true;
        }
        return v;
    }
    std::string str() {
        const uint64_t n = get<uint64_t>();
        if (bad || n > (1u << 24)) {
            bad = true;
            return {};
        }
        std::string s(n, '\0');
        if (n && std::fread(s.data(), 1, n, f) != n) {
            bad = true;
        }
        return s;
    }
    void skip(uint64_t n) {
        if (std::fseek(f, (long) n, SEEK_CUR) != 0) {
            bad = true;
        }
    }
};

uint64_t scalar_size(uint32_t t) {
    switch (t) {
        case U8: case I8: case BOOL: return 1;
        case U16: case I16: return 2;
        case U32: case I32: case F32: return 4;
        case U64: case I64: case F64: return 8;
        default: return 0;
    }
}

// A scalar as an integer; 0 for strings and arrays.
uint64_t read_int(Reader & r, uint32_t t) {
    switch (t) {
        case U8: return r.get<uint8_t>();
        case I8: return (uint64_t) r.get<int8_t>();
        case U16: return r.get<uint16_t>();
        case I16: return (uint64_t) r.get<int16_t>();
        case U32: return r.get<uint32_t>();
        case I32: return (uint64_t) r.get<int32_t>();
        case U64: return r.get<uint64_t>();
        case I64: return (uint64_t) r.get<int64_t>();
        case BOOL: return r.get<uint8_t>();
        case F32: return (uint64_t) r.get<float>();
        case F64: return (uint64_t) r.get<double>();
        default: return 0;
    }
}

std::string lower(std::string s) {
    for (auto & c : s) {
        c = (char) std::tolower((unsigned char) c);
    }
    return s;
}

}  // namespace

ModelInfo read_model_info(const std::string & path) {
    ModelInfo m;
    m.path      = path;
    m.file_name = path.substr(path.find_last_of('/') + 1);
    m.name      = m.file_name;

    FILE * f = std::fopen(path.c_str(), "rb");
    if (!f) {
        m.error = "cannot open";
        return m;
    }
    Reader r{ f };
    if (r.get<uint32_t>() != 0x46554747u) {  // "GGUF"
        std::fclose(f);
        m.error = "not a GGUF file";
        return m;
    }
    r.get<uint32_t>();  // version
    r.get<uint64_t>();  // tensor count
    const uint64_t n_kv = r.get<uint64_t>();

    std::map<std::string, uint64_t> ints;  // every scalar, and the largest element of numeric arrays
    std::map<std::string, std::string> strs;
    for (uint64_t i = 0; i < n_kv && !r.bad; i++) {
        const std::string key = r.str();
        const uint32_t    t   = r.get<uint32_t>();
        if (t == STRING) {
            strs[key] = r.str();
        } else if (t == ARRAY) {
            const uint32_t et = r.get<uint32_t>();
            const uint64_t n  = r.get<uint64_t>();
            if (et == STRING) {
                for (uint64_t j = 0; j < n && !r.bad; j++) {
                    r.skip(r.get<uint64_t>());
                }
            } else if (n <= 4096) {  // per-layer values, such as head_count_kv
                uint64_t mx = 0;
                for (uint64_t j = 0; j < n; j++) {
                    mx = std::max(mx, read_int(r, et));
                }
                ints[key] = mx;
            } else {
                r.skip(n * scalar_size(et));  // vocabularies and the like
            }
        } else {
            ints[key] = read_int(r, t);
        }
    }
    std::fseek(f, 0, SEEK_END);
    m.file_bytes = (uint64_t) std::ftell(f);
    std::fclose(f);
    if (r.bad) {
        m.error = "metadata could not be read";
        return m;
    }

    m.arch = strs["general.architecture"];
    if (!strs["general.name"].empty()) {
        m.name = strs["general.name"];
    }
    const std::string a = m.arch + ".";
    auto num = [&](const std::string & k, uint64_t def = 0) {
        auto it = ints.find(a + k);
        return it == ints.end() ? def : it->second;
    };
    // block_count includes the MTP blocks some files carry (Qwen 3.8's
    // UD-Q2_K_XL has 65); they are not part of the main stack.
    m.n_layer         = (uint32_t) (num("block_count") - num("nextn_predict_layers"));
    const uint64_t n_head = num("attention.head_count");
    m.n_head_kv       = (uint32_t) num("attention.head_count_kv", n_head);
    const uint64_t n_embd = num("embedding_length");
    const uint64_t head_dim = n_head ? n_embd / n_head : 0;
    m.key_length      = (uint32_t) num("attention.key_length", head_dim);
    m.value_length    = (uint32_t) num("attention.value_length", head_dim);
    m.full_attn_every = (uint32_t) std::max<uint64_t>(1, num("full_attention_interval", 1));
    m.n_ctx_train     = (uint32_t) num("context_length");
    m.n_expert        = (uint32_t) num("expert_count");

    // Recurrent layers (Gated DeltaNet, Mamba): a fixed f32 state each,
    // the recurrent matrix plus the convolution window.
    const uint64_t inner = num("ssm.inner_size"), state = num("ssm.state_size");
    if (inner && state) {
        const uint64_t groups = num("ssm.group_count", 1), conv = num("ssm.conv_kernel", 4);
        const uint64_t n_rec  = m.n_layer - m.n_kv_layers();
        m.state_bytes = n_rec * (inner * state + (conv - 1) * (inner + 2 * groups * state)) * 4;
    }
    m.ok = m.n_layer > 0;
    if (!m.ok) {
        m.error = "no layer count for architecture '" + m.arch + "'";
    }
    return m;
}

double kv_bytes_per_token(const ModelInfo & m, const std::string & type) {
    const double per_elem = type == "f16" ? 2.0 : type == "q8_0" ? 34.0 / 32 : 18.0 / 32;  // q4_0
    return (double) m.n_kv_layers() * m.n_head_kv * (m.key_length + m.value_length) * per_elem;
}

// Sampling as each model's authors recommend it. Settings the planner works
// out (context, cache type) are not here.
const std::vector<Preset> & presets() {
    static const std::vector<Preset> table = {
        { "qwen3.8", "Qwen 3.8", "27B hybrid Gated DeltaNet; UD-Q2_K_XL holds 64k context on the GPU",
          { "--temp", "0.6", "--top-p", "0.95", "--top-k", "20", "--min-p", "0" } },
        { "qwen3.6", "Qwen 3.6", "MoE 35B-A3B or dense 27B; the MoE decodes fastest",
          { "--temp", "0.6", "--top-p", "0.95", "--top-k", "20", "--min-p", "0" } },
        { "qwen3.5", "Qwen 3.5", "hybrid like 3.8; 9B at Q4_K_M leaves room for long context",
          { "--temp", "0.6", "--top-p", "0.95", "--top-k", "20", "--min-p", "0" } },
        { "ornith", "Ornith 1.5", "Qwen 3.5 based 9B",
          { "--temp", "0.6", "--top-p", "0.95", "--top-k", "20", "--min-p", "0" } },
        { "gemma", "Gemma 4", "12B at Q3_K_M / IQ4_XS, E4B at Q4_0",
          { "--temp", "1.0", "--top-p", "0.95", "--top-k", "64" } },
        { "gpt-oss", "gpt-oss", "20B MoE, 3.6B active; MXFP4 is its only size, so context stays short",
          { "--temp", "1.0", "--top-p", "1.0" }, 16384 },
        { "granite", "Granite 4.2", "3B and 8B, fast and plain",
          { "--temp", "0.0" } },
        { "nemotron", "Nemotron 3 Nano", "hybrid Mamba, 1M context trained",
          { "--temp", "0.6", "--top-p", "0.95" } },
        { "mistral", "Mistral", "7B v0.3 and the Small line",
          { "--temp", "0.15" } },
        { "llama", "Llama 3", "8B fits at any 4-bit quant",
          { "--temp", "0.6", "--top-p", "0.9" } },
    };
    return table;
}

const Preset * find_preset(const ModelInfo & m) {
    const std::string hay = lower(m.file_name + " " + m.name + " " + m.arch);
    for (const auto & p : presets()) {
        if (hay.find(p.match) != std::string::npos) {
            return &p;
        }
    }
    return nullptr;
}

Plan plan_model(const ModelInfo & m, double budget_gib, uint32_t ctx_cap, const char * kv_force) {
    Plan p;
    const Preset * preset = find_preset(m);
    if (preset && preset->max_ctx) {
        ctx_cap = std::min(ctx_cap, preset->max_ctx);
    }
    if (m.n_ctx_train) {
        ctx_cap = std::min(ctx_cap, m.n_ctx_train);
    }
    p.weights_gib = m.file_bytes / kGiB;
    p.state_gib   = m.state_bytes / kGiB;
    // Compute buffers and the CPU side (the title heap shares the pool), plus
    // one prompt checkpoint of the recurrent state. Calibrated on the console:
    // Qwen3.8-27B UD-Q2_K_XL at 64k with a q4_0 cache ran with 11.38 GiB free.
    p.reserve_gib = 0.75 + p.state_gib;

    // The longest context first, and at each length the best cache that fits.
    static const uint32_t ctxs[]  = { 131072, 65536, 32768, 16384, 8192, 4096 };
    static const char *   types[] = { "f16", "q8_0", "q4_0" };
    for (uint32_t ctx : ctxs) {
        if (ctx > ctx_cap) {
            continue;
        }
        for (const char * t : types) {
            if (kv_force && std::strcmp(t, kv_force) != 0) {
                continue;
            }
            // f16 only where it is cheap: q8_0 costs nothing measurable in quality.
            if (!kv_force && std::strcmp(t, "f16") == 0 && ctx > 8192) {
                continue;
            }
            const double kv    = kv_bytes_per_token(m, t) * ctx / kGiB;
            const double total = p.weights_gib + kv + p.state_gib + p.reserve_gib;
            if (total <= budget_gib) {
                p.fits = true;
                p.ctx = ctx;
                p.kv_type = t;
                p.kv_gib = kv;
                p.total_gib = total;
                break;
            }
        }
        if (p.fits) {
            break;
        }
    }

    char why[160];
    if (!p.fits) {
        p.total_gib = p.weights_gib + p.state_gib + p.reserve_gib;
        std::snprintf(why, sizeof(why), "needs %.1f GiB with a 4k context; %.1f GiB is free", p.total_gib, budget_gib);
        p.why = why;
        return p;
    }
    std::snprintf(why, sizeof(why), "%uk context, %s cache: %.1f of %.1f GiB", p.ctx / 1024, p.kv_type.c_str(),
                  p.total_gib, budget_gib);
    p.why = why;

    p.args = { "-m", m.path, "-ngl", "999", "-fit", "off", "-lm", "none",
               "-c", std::to_string(p.ctx), "-fa", "on", "-ctk", p.kv_type, "-ctv", p.kv_type,
               "-b", "256", "-ub", "64", "-np", "1",
               // One prompt checkpoint and no RAM cache: llama-server's defaults
               // (32 checkpoints, 8 GiB) are for a PC, and the CPU shares this pool.
               "-ctxcp", "1", "-cram", "0",
               "--host", "0.0.0.0", "--port", "8081" };
    if (preset) {
        p.args.insert(p.args.end(), preset->args.begin(), preset->args.end());
    }
    return p;
}

std::vector<std::string> find_models(const std::vector<std::string> & dirs) {
    std::vector<std::string> out;
    for (const auto & d : dirs) {
        DIR * dir = opendir(d.c_str());
        if (!dir) {
            continue;
        }
        while (dirent * e = readdir(dir)) {
            const std::string n = e->d_name;
            if (n.size() < 6 || lower(n.substr(n.size() - 5)) != ".gguf" || n[0] == '.') {
                continue;
            }
            // Split models: only the first part, which llama.cpp opens.
            const auto split = n.find("-of-");
            if (split != std::string::npos && n.find("-00001-of-") == std::string::npos) {
                continue;
            }
            // Vision projectors are not models on their own.
            if (lower(n).find("mmproj") != std::string::npos) {
                continue;
            }
            out.push_back(d + "/" + n);
        }
        closedir(dir);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace ps5lm
