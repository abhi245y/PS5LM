// PS5LM native app: llama-server on the PS5's GPU through RADV (ggml-vulkan),
// with a dashboard on the TV drawn by ps5-homebrew-ui through ps5-opengl.
//
// A title gets the console's direct memory (12 GiB) where a payload gets none,
// and RADV turns it into Vulkan device memory, so the whole model sits on the
// GPU. llama-server runs on its own thread on port 8081 (chat page and OpenAI
// API); the main thread draws the dashboard, reads the controller and plays
// the interface sounds. Models come from /data/PS5LM/models and USB drives; the
// planner (model_plan.cpp) picks each one's context and cache for the memory
// free. /data/PS5LM/app-args.txt, one argument per line, overrides all of it.
// The app logs to /data/PS5LM/app.log and llama.log.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <GL/glcorearb.h>

#include "dashboard.hpp"
#include "display.hpp"
#include "ggml-backend.h"
#include "model_plan.hpp"
#include "settings.hpp"
#include "stats.hpp"
#include "version.hpp"

#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>

#include "audio/mixer.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

int  llama_server(int argc, char ** argv);
void llama_server_terminate();

// ---- Vulkan without a loader ----------------------------------------------------
// RADV is linked in whole, and ggml-vulkan's dispatcher starts from
// vkGetInstanceProcAddr, which the ICD entry point answers for every command.
// The few commands ggml calls by name go to Mesa's common implementations,
// which dispatch through the object's own table.
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char * name);
extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char * name) {
    return vk_icdGetInstanceProcAddr(instance, name);
}
extern "C" PFN_vkVoidFunction vk_common_GetDeviceProcAddr(VkDevice device, const char * name);
extern "C" void vk_common_GetPhysicalDeviceFeatures2(VkPhysicalDevice dev, VkPhysicalDeviceFeatures2 * features);
extern "C" void vk_common_CmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t n,
                                        const VkBufferCopy * regions);
extern "C" PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char * name) {
    return vk_common_GetDeviceProcAddr(device, name);
}
extern "C" void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice dev, VkPhysicalDeviceFeatures2 * features) {
    vk_common_GetPhysicalDeviceFeatures2(dev, features);
}
extern "C" void vkCmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t n,
                                const VkBufferCopy * regions) {
    vk_common_CmdCopyBuffer(cmd, src, dst, n, regions);
}

extern "C" {
int sceSystemServiceHideSplashScreen(void);
int sceKernelSendNotificationRequest(uint32_t device, void * request, size_t size, int blocking);
int sceKernelUsleep(uint32_t microseconds);
int sceSystemServiceLoadExec(const char * path, const char * const * args);
}

namespace {

using namespace ps5lm;

constexpr const char * kLog         = "/data/PS5LM/app.log";
constexpr const char * kArgs        = "/data/PS5LM/app-args.txt";
constexpr const char * kChoice      = "/data/PS5LM/model.txt";
constexpr const char * kQuit        = "/data/PS5LM/quit";
constexpr const char * kSettings    = "/data/PS5LM/settings.json";
constexpr const char * kLlamaLog    = "/data/PS5LM/llama.log";
constexpr const char * kModels      = "/data/PS5LM/models";
constexpr const char * kAssets      = "/app0/assets";
constexpr int          kPort        = 8081;
const std::vector<std::string> kModelDirs = { kModels, "/mnt/usb0/PS5LM/models", "/mnt/usb1/PS5LM/models",
                                              "/mnt/usb0", "/mnt/usb1" };

// The last lines of a log, up to about 64 KiB of it, oldest first.
std::vector<std::string> tail_lines(const char * path) {
    std::vector<std::string> lines;
    FILE * f = std::fopen(path, "rb");
    if (!f) {
        return lines;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    const long from = std::max(0L, size - 65536L);
    std::fseek(f, from, SEEK_SET);
    std::string text((size_t) (size - from), '\0');
    text.resize(std::fread(text.data(), 1, text.size(), f));
    std::fclose(f);
    size_t start = from > 0 ? text.find('\n') + 1 : 0;  // the first line is cut
    for (size_t end; start < text.size() && (end = text.find('\n', start)) != std::string::npos; start = end + 1) {
        lines.emplace_back(text, start, end - start);
    }
    return lines;
}

// A system notification on the TV.
void notify(const char * text) {
    struct {
        uint8_t reserved[45];
        char    message[3075];
    } request = {};
    std::snprintf(request.message, sizeof(request.message), "%s", text);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

// A quit request from the PC (scripts/ps5lm-app.sh close): the app ends
// itself the way the system expects; killing a title that is rendering has
// been followed by lost consoles (ps5-homebrew-ui's AGENTS.md).
void quit_if_asked() {
    if (access(kQuit, F_OK) != 0) {
        return;
    }
    unlink(kQuit);
    std::printf("ps5lm-app: quit requested\n");
    std::fflush(nullptr);
    sceSystemServiceLoadExec("exit", nullptr);
}

// The screen as a 24-bit BMP (glReadPixels of the TV's framebuffer), for
// checking the dashboard from the PC.
bool save_screen(const char * path, int width, int height) {
    std::vector<unsigned char> pixels((size_t) width * height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const uint32_t row = ((uint32_t) width * 3 + 3) & ~3u;
    const uint32_t size = 54 + row * (uint32_t) height;
    unsigned char header[54] = { 'B', 'M' };
    auto put = [&](int at, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            header[at + i] = (unsigned char) (v >> (8 * i));
        }
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, (uint32_t) width);
    put(22, (uint32_t) height);
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    FILE * f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = std::fwrite(header, 1, sizeof(header), f) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; ok && y < height; ++y) {
        const unsigned char * in = pixels.data() + (size_t) y * width * 4;
        for (int x = 0; x < width; ++x) {
            line[(size_t) x * 3 + 0] = in[x * 4 + 2];
            line[(size_t) x * 3 + 1] = in[x * 4 + 1];
            line[(size_t) x * 3 + 2] = in[x * 4 + 0];
        }
        ok = std::fwrite(line.data(), 1, row, f) == row;
    }
    std::fclose(f);
    return ok;
}

// A request file from the PC: its first line, and the file is consumed.
std::string take_request_file(const char * path) {
    if (access(path, F_OK) != 0) {
        return {};
    }
    std::string line;
    std::ifstream(path) >> line;
    unlink(path);
    return line.empty() ? std::string("-") : line;
}

// Where the memory is: the pool's free part, the CPU heap, and what ggml
// says about each device (its GPU figure wraps once the unified heap is
// over-committed; the raw numbers still show a change).
void log_memory(const char * when) {
    int64_t at = 0;
    size_t  avail = 0;
    const int64_t pool = sceKernelGetDirectMemorySize();
    sceKernelAvailableDirectMemorySize(0, pool, 2 * 1024 * 1024, &at, &avail);
    struct ps5_heap_stats heap = {};
    ps5_heap_stats(&heap);
    std::printf("ps5lm-app: memory %s: pool free %.2f GiB, cpu heap %.2f GiB (peak %.2f)", when, avail / 1073741824.0,
                heap.mapped_bytes / 1073741824.0, heap.peak_bytes / 1073741824.0);
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        size_t free = 0, total = 0;
        ggml_backend_dev_memory(ggml_backend_dev_get(i), &free, &total);
        std::printf(", %s free %zu total %zu", ggml_backend_dev_name(ggml_backend_dev_get(i)), free, total);
    }
    std::printf("\n");
}

std::vector<std::string> read_lines(const char * path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
            out.push_back(line);
        }
    }
    return out;
}

bool is_usb(const std::string & path) { return path.rfind("/mnt/usb", 0) == 0; }

std::string base_name(const std::string & path) {
    std::string n = path.substr(path.find_last_of('/') + 1);
    if (n.size() > 5 && n.compare(n.size() - 5, 5, ".gguf") == 0) {
        n.resize(n.size() - 5);
    }
    return n;
}

// ---- the server thread ------------------------------------------------------------

std::atomic<int> g_server_rc{ -1 };   // llama-server's exit code once it returns; -1 while it runs
std::vector<std::string> g_server_args;

void * run_server(void *) {
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>("llama-server"));
    for (auto & a : g_server_args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    int rc = 1;
    try {
        rc = llama_server((int) argv.size() - 1, argv.data());
    } catch (const std::exception & e) {
        std::printf("ps5lm-app: exception: %s\n", e.what());
    } catch (...) {
        std::printf("ps5lm-app: unknown exception\n");
    }
    std::printf("ps5lm-app: llama_server returned %d\n", rc);
    std::fflush(nullptr);
    g_server_rc = rc == 0 ? 0 : 1;
    return nullptr;
}

// ---- models -------------------------------------------------------------------------

struct Models {
    double                 budget_gib = 0;   // GPU memory free before any model
    std::vector<ModelRow>  rows;
    std::string            running;          // path of the model llama-server has
    pthread_t              server{};
    bool                   server_started = false;
    Settings               settings;

    const char * kv_force() const { return settings.kv_type == "auto" ? nullptr : settings.kv_type.c_str(); }

    void scan(StatsCollector & stats) {
        rows.clear();
        for (const auto & path : find_models(kModelDirs)) {
            const ModelInfo m = read_model_info(path);
            ModelRow row;
            row.file = path;
            row.label = base_name(path);
            row.on_usb = is_usb(path);
            if (!m.ok) {
                row.plan = m.error;
                rows.push_back(row);
                continue;
            }
            const Plan p = plan_model(m, budget_gib, settings.ctx_cap, kv_force());
            const Preset * pre = find_preset(m);
            row.arch = m.arch;
            row.plan = p.why;
            row.fits = p.fits;
            row.size_gib = m.file_bytes / 1073741824.0;
            row.preset = pre ? pre->label : "";
            row.current = path == running;
            rows.push_back(row);
            std::printf("ps5lm-app: model %s (%s): %s\n", m.file_name.c_str(), m.arch.c_str(), p.why.c_str());
        }
        publish(stats);
    }

    void publish(StatsCollector & stats) {
        std::vector<ModelRow> out = rows;
        for (auto & r : out) {
            r.current = r.file == running;
        }
        stats.set_models(out);
    }

    // The arguments for a model: the planner's, or app-args.txt's verbatim.
    std::vector<std::string> args_for(const std::string & path, Live & model_part) {
        const ModelInfo m = read_model_info(path);
        const Plan p = plan_model(m, budget_gib, settings.ctx_cap, kv_force());
        const Preset * pre = find_preset(m);
        model_part.model_label = base_name(path);
        model_part.model_file  = path;
        model_part.plan        = p.why;
        model_part.args        = p.args;
        model_part.model_arch  = m.arch;
        model_part.preset      = pre ? pre->label : "";
        model_part.model_gib   = m.file_bytes / 1073741824.0;
        model_part.ctx         = p.ctx;
        model_part.kv_type     = p.kv_type;
        model_part.kv_gib      = p.kv_gib;
        return p.fits ? p.args : std::vector<std::string>{};
    }

    void start(const std::string & path, std::vector<std::string> args, StatsCollector & stats, const Live & model_part) {
        // llama.cpp's own log, written as it goes (it survives a crash), and
        // the metrics the dashboard reads.
        args.insert(args.begin(), { "--log-file", "/data/PS5LM/llama.log" });
        if (std::find(args.begin(), args.end(), "--metrics") == args.end()) {
            args.push_back("--metrics");
        }
        for (const auto & a : args) {
            std::printf("ps5lm-app: arg %s\n", a.c_str());
        }
        g_server_args = args;
        g_server_rc = -1;
        running = path;
        stats.set_model(model_part);
        stats.set_state(ServerState::loading);
        publish(stats);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16u << 20);  // the console's default stack is small
        pthread_create(&server, &attr, run_server, nullptr);
        server_started = true;
        notify("PS5LM: loading the model onto the GPU");
    }

    void stop() {
        if (!server_started) {
            return;
        }
        llama_server_terminate();
        // With a reply in flight llama-server can stay inside its HTTP thread
        // pool forever (unload during generation hung the app). We are about
        // to be replaced by LoadExec anyway, so wait 10 s at most.
        for (int i = 0; i < 100 && g_server_rc < 0; ++i) {
            usleep(100 * 1000);
        }
        if (g_server_rc >= 0) {
            pthread_join(server, nullptr);
        } else {
            std::printf("ps5lm-app: llama_server did not return in 10 s; restarting anyway\n");
        }
        server_started = false;
        running.clear();
        log_memory("after unloading");
    }
};

bool load_font(hui::gfx::Renderer & renderer, const char * name, hui::gfx::Font * font, hui::ui::FontRef * ref) {
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data)) {
        std::printf("ps5lm-app: font %s failed\n", name);
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

}  // namespace

int main(int, char **) {
    // stdout and stderr to a file the PC can fetch over FTP.
    if (std::freopen(kLog, "w", stdout) != nullptr) {
        setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    if (std::freopen(kLog, "a", stderr) != nullptr) {
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    std::printf("ps5lm-app: version " PS5LM_VERSION "\n");
    // One device-local heap over the whole pool (RADV splits an APU's 2/3 to
    // 1/3), and a HOME for llama.cpp, which throws without one.
    setenv("radv_enable_unified_heap_on_apu", "true", 1);
    setenv("HOME", "/data/PS5LM", 0);
    setenv("LLAMA_CACHE", "/data/PS5LM/cache", 0);
    mkdir(kModels, 0777);
    std::printf("ps5lm-app: start\n");

    // The screen first: it takes from the same memory as the model.
    Display display;
    if (!display.open(1920, 1080)) {
        std::printf("ps5lm-app: no display\n");
    }
    hui::gfx::Renderer renderer;
    hui::gfx::Font regular, semibold, headline, mono;
    hui::ui::Fonts fonts;
    const bool ui_ok = renderer.init() && load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) &&
                       load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) &&
                       load_font(renderer, "montserrat-medium.huifont", &headline, &fonts.display) &&
                       load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono);
    fonts.pixel = fonts.mono;
    fonts.hand = fonts.regular;
    std::printf("ps5lm-app: dashboard %s\n", ui_ok ? "ready" : "failed");

    Models models;
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        size_t free = 0, total = 0;
        ggml_backend_dev_memory(dev, &free, &total);
        std::printf("ps5lm-app: device %zu %s (%s): %.2f / %.2f GiB free\n", i, ggml_backend_dev_name(dev),
                    ggml_backend_dev_description(dev), free / 1073741824.0, total / 1073741824.0);
        // The PS5 is an APU: ggml calls its GPU integrated.
        const auto type = ggml_backend_dev_type(dev);
        if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU) {
            models.budget_gib = free / 1073741824.0;
        }
    }

    StatsCollector stats;
    stats.start(kPort);
    models.settings = load_settings(kSettings);
    models.scan(stats);

    // Which model: app-args.txt verbatim, else model.txt, which the library
    // writes just before restarting the app for a switch and which is used
    // once, else the default model when Settings asks for it at launch. A
    // plain launch otherwise loads nothing and opens the library.
    Live model_part;
    std::vector<std::string> args = read_lines(kArgs);
    std::string path;
    if (!args.empty()) {
        const auto m = std::find(args.begin(), args.end(), "-m");
        path = m != args.end() && m + 1 != args.end() ? *(m + 1) : "";
        model_part.model_label = base_name(path);
        const ModelInfo info = read_model_info(path);
        model_part.model_arch = info.arch;
        model_part.model_gib = info.file_bytes / 1073741824.0;
        model_part.model_file = path;
        model_part.args = args;
        std::printf("ps5lm-app: app-args.txt overrides the planner\n");
    } else {
        auto chosen = read_lines(kChoice);
        unlink(kChoice);
        if (chosen.empty() && models.settings.auto_load && !models.settings.default_model.empty()) {
            chosen = { models.settings.default_model };  // "none" after an unload keeps it from coming back
        }
        for (const auto & r : models.rows) {
            if (r.fits && !chosen.empty() && r.file == chosen[0]) {
                path = r.file;
            }
        }
        if (!path.empty()) {
            args = models.args_for(path, model_part);
        }
    }
    log_memory("before loading");
    if (!args.empty()) {
        models.start(path, args, stats, model_part);
    } else {
        stats.set_state(ServerState::no_model);
        if (models.rows.empty()) {
            notify("PS5LM: no models; copy a .gguf to /data/PS5LM/models or PS5LM/models on a USB drive");
        }
    }

    // Input and sound, as ps5-homebrew-ui's own app does.
    hui::ps5::Pad pad;
    pad.open();
    hui::InputTracker tracker;
    hui::audio::Mixer mixer;
    hui::ps5::AudioOut audio_out;
    audio_out.start(mixer);
    hui::audio::SoundBank sounds;
    sounds.load(std::string(kAssets) + "/audio/sfx");

    Dashboard dash(fonts);
    dash.set_settings(models.settings);
    bool      library_shown = false;  // opened once at launch when no model is loaded
    DashboardFrame frame;
    hui::ui::Feedback feedback;
    hui::PadSample samples[64];
    int64_t last = hui::sys::monotonic_us();
    bool was_serving = false;
    for (uint64_t n = 0;; ++n) {
        const int64_t now = hui::sys::monotonic_us();
        const float dt = n == 0 ? 1.0f / 60.0f : std::min(0.05f, (float) (now - last) / 1e6f);
        last = now;

        const size_t count = pad.read(samples);
        const hui::InputFrame input = tracker.update(std::span<const hui::PadSample>(samples, count), (uint64_t) now);
        Live live = stats.snapshot();
        if (g_server_rc >= 0 && live.state != ServerState::failed && live.state != ServerState::no_model) {
            stats.set_state(ServerState::failed);
            notify("PS5LM: the server stopped, see /data/PS5LM/llama.log");
        }
        const bool serving = live.state == ServerState::ready || live.state == ServerState::generating;
        if (serving && !was_serving) {
            notify("PS5LM: ready, chat in a browser on this network");
        }
        was_serving = serving;
        if (n % 30 == 0) {
            models.publish(stats);
        }
        dash.set_live(live);
        if (!library_shown && !live.models.empty()) {
            library_shown = true;
            if (live.state == ServerState::no_model) {
                dash.open_library();
            }
        }
        feedback.clear();
        // A button pressed from the PC (scripts/ps5lm-app.sh press r1), for
        // testing the screens without a controller in hand.
        hui::InputFrame in = input;
        if (n % 20 == 10) {
            const std::string key = take_request_file("/data/PS5LM/press");
            const struct {
                const char *   name;
                hui::Action    action;
                hui::Direction nav;
            } keys[] = {
                { "up", hui::Action::up, hui::Direction::up },
                { "down", hui::Action::down, hui::Direction::down },
                { "left", hui::Action::left, hui::Direction::left },
                { "right", hui::Action::right, hui::Direction::right },
                { "cross", hui::Action::confirm, hui::Direction::none },
                { "circle", hui::Action::back, hui::Direction::none },
                { "triangle", hui::Action::north, hui::Direction::none },
                { "square", hui::Action::west, hui::Direction::none },
                { "l1", hui::Action::page_prev, hui::Direction::none },
                { "r1", hui::Action::page_next, hui::Direction::none },
            };
            for (const auto & k : keys) {
                if (key == k.name) {
                    in.connected = true;
                    in.pressed |= hui::action_bit(k.action);
                    in.nav = k.nav;
                }
            }
        }
        dash.update(in, dt, feedback);
        for (const auto & cue : feedback.cues) {
            if (!models.settings.sounds) {
                break;
            }
            sounds.play(mixer, cue.set == hui::audio::SoundSet::count ? hui::audio::SoundSet::glass : cue.set, cue);
        }
        if (feedback.rumble_strength > 0.0f) {
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        }
        pad.tick(dt);

        if (dash.page() == Dashboard::kLogsPage && n % 60 == 0) {
            dash.set_logs(tail_lines(kLog), tail_lines(kLlamaLog));
        }

        Dashboard::Request req = dash.take_request();
        if (req.kind == Dashboard::Request::settings) {
            // The planner's limits change every row's plan; the loaded model
            // keeps its settings until it is loaded again.
            models.settings = dash.settings();
            save_settings(kSettings, models.settings);
            models.scan(stats);
            std::printf("ps5lm-app: settings saved\n");
        }
        // The same requests from the PC (scripts/ps5lm-app.sh load|unload).
        if (n % 60 == 30 && req.kind == Dashboard::Request::none) {
            if (std::string p = take_request_file("/data/PS5LM/load"); !p.empty()) {
                req = { Dashboard::Request::load, p };
            } else if (!take_request_file("/data/PS5LM/unload").empty()) {
                req = { Dashboard::Request::unload, "" };
            }
        }
        if (req.kind == Dashboard::Request::unload || req.kind == Dashboard::Request::load) {
            // Unloading in place leaves part of the model's GPU memory held
            // (about 5 GiB of a 6.8 GiB model, measured), so a model change
            // restarts the app: the system replaces the process (LoadExec on
            // our own eboot.bin, "Kill for LoadExec" in the log) and the new
            // one starts from the whole pool, with model.txt saying what to load.
            std::ofstream(kChoice) << (req.kind == Dashboard::Request::load ? req.path : std::string("none")) << "\n";
            if (req.kind == Dashboard::Request::load) {
                unlink(kArgs);  // the library's choice replaces a manual override
            }
            notify(req.kind == Dashboard::Request::load ? "PS5LM: loading the new model" : "PS5LM: model unloaded");
            std::printf("ps5lm-app: restarting for %s\n", req.kind == Dashboard::Request::load ? req.path.c_str() : "no model");
            std::fflush(nullptr);
            models.stop();
            sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
        }

        if (ui_ok) {
            frame.backdrop = {};
            frame.scene.clear();
            frame.overlay.clear();
            frame.glass = false;
            frame.glass_texture = renderer.glass_texture();
            dash.draw(frame);
            renderer.begin();
            renderer.backdrop(frame.backdrop);
            renderer.draw(frame.scene);
            if (frame.glass) {
                renderer.glass();
            }
            renderer.draw(frame.overlay);
            display.present(renderer);
            if (n % 60 == 45 && !take_request_file("/data/PS5LM/screenshot").empty()) {
                const bool ok = save_screen("/data/PS5LM/screen.bmp", display.width(), display.height());
                std::printf("ps5lm-app: screenshot %s\n", ok ? "saved" : "failed");
            }
        }
        display.swap();
        if (n == 0) {
            sceSystemServiceHideSplashScreen();  // the first picture is up
        }
        if (n % 60 == 0) {
            quit_if_asked();
        }
    }
}
