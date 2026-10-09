// PC preview of the PS5LM dashboard: the same drawing code as the console,
// rendered off-screen through Mesa's surfaceless EGL to PNG files, with
// sample data shaped like a real run (Qwen 3.8 27B at 64k on the GPU).
//
//   scripts/preview-app-ui.sh            writes build/app-preview/*.png
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard.hpp"

#include "core/save_file.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool open_context() {
    auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                                     : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API)) {
        return false;
    }
    const EGLint attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                             EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    return context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer & r, const std::string & path, hui::gfx::Font * font, hui::ui::FontRef * ref) {
    std::string data;
    if (!hui::save::read_file(path, &data) || !font->load(data)) {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font    = font;
    ref->texture = r.batch().create_font_texture(*font);
    return true;
}

// A run as the console measured it: Qwen3.8-27B UD-Q2_K_XL, 64k, q4_0 cache.
float g_loading = -1;  // >= 0: draw the loading state at this progress

ps5lm::Live sample(float seconds, bool generating) {
    ps5lm::Live L;
    L.state       = generating ? ps5lm::ServerState::generating : ps5lm::ServerState::ready;
    L.address     = "http://192.168.1.19:8081";
    L.model_label = "Qwen3.8-27B-UD-Q2_K_XL";
    L.model_arch  = "qwen35";
    L.preset      = "Qwen 3.8";
    L.model_gib   = 9.15;
    L.ctx         = 65536;
    L.kv_type     = "q4_0";
    L.kv_gib      = 1.13;
    L.model_file  = "/data/PS5LM/models/Qwen3.8-27B-UD-Q2_K_XL.gguf";
    L.plan        = "64k context, q4_0 cache: 11.3 of 11.4 GiB";
    L.args = { "-m", L.model_file, "-ngl", "999", "-fit", "off", "-lm", "none", "-c", "65536", "-fa", "on",
               "-ctk", "q4_0", "-ctv", "q4_0", "-b", "256", "-ub", "64", "-np", "1", "-ctxcp", "1", "-cram", "0",
               "--host", "0.0.0.0", "--port", "8081", "--temp", "0.6", "--top-p", "0.95", "--top-k", "20",
               "--min-p", "0", "--metrics" };
    L.soc_temp    = 61;
    L.cpu_temp    = 54;
    L.soc_power_w = 118;
    L.cpu_ghz     = 3.5f;
    L.data_free   = 412;
    L.data_total  = 825;
    L.usb_free    = 455;
    L.usb_total   = 477;
    L.pool_gib    = 11.44;
    L.free_gib    = 0.31;
    L.heap_gib    = 0.76;
    L.cpus        = 13;
    L.uptime_s    = 754 + seconds;
    L.tokens      = 4182;
    L.prompt_tokens = 9120;
    L.requests    = 14;
    L.ctx_used    = 2318;
    L.prompt_tps  = 45.4f;
    for (int i = 0; i < 120; ++i) {
        const bool on = (i / 14) % 3 != 0 || i > 104;
        const float tps = on ? 20.4f + 0.6f * std::sin(i * 0.7f) : 0.0f;
        L.gen_history.push_back(tps);
        L.gpu_history.push_back(on ? 0.92f + 0.05f * std::sin(i * 0.3f) : 0.03f);
        L.cpu_history.push_back(on ? 0.11f + 0.03f * std::sin(i * 0.5f) : 0.02f);
        L.temp_history.push_back(on ? 60.0f + 2.0f * std::sin(i * 0.2f) : 52.0f);
    }
    L.gen_tps  = generating ? 20.7f : 0.0f;
    L.gpu_busy = generating ? 0.94f : 0.02f;
    L.cpu_use  = generating ? 0.12f : 0.02f;
    const char * files[] = { "Qwen3.8-27B-UD-IQ1_S", "Qwen3.8-27B-UD-IQ2_S", "Qwen3.8-27B-UD-IQ2_XXS",
                             "Qwen3.8-27B-UD-Q2_K_XL", "gpt-oss-20b-MXFP4" };
    const char * plans[] = { "64k context, q8_0 cache: 8.9 of 11.0 GiB", "64k context, q8_0 cache: 11.0 of 11.0 GiB",
                             "64k context, q8_0 cache: 9.9 of 11.0 GiB", "64k context, q4_0 cache: 11.3 of 11.4 GiB",
                             "needs 11.6 GiB with a 4k context; 11.0 GiB is free" };
    const double sizes[] = { 5.77, 7.80, 6.77, 9.15, 11.3 };
    for (int i = 0; i < 5; ++i) {
        ps5lm::ModelRow m;
        m.file = std::string("/data/PS5LM/models/") + files[i] + ".gguf";
        m.label = files[i];
        m.plan = plans[i];
        m.size_gib = sizes[i];
        m.fits = i < 4;
        m.current = i == 3;
        m.preset = i < 4 ? "Qwen 3.8" : "gpt-oss";
        L.models.push_back(m);
    }
    ps5lm::ModelRow usb;
    usb.file = "/mnt/usb0/PS5LM/models/Ornith-1.5-9B-Q4_K_M.gguf";
    usb.label = "Ornith-1.5-9B-Q4_K_M";
    usb.plan = "64k context, q8_0 cache: 8.6 of 11.0 GiB";
    usb.preset = "Ornith 1.5";
    usb.size_gib = 5.38;
    usb.fits = usb.on_usb = true;
    L.models.push_back(usb);
    usb.file = "/mnt/usb0/PS5LM/models/gemma-4-12b-it-Q3_K_M.gguf";
    usb.label = "gemma-4-12b-it-Q3_K_M";
    usb.plan = "64k context, q8_0 cache: 9.1 of 11.0 GiB";
    usb.preset = "Gemma 4";
    usb.size_gib = 5.30;
    L.models.push_back(usb);
    if (g_loading >= 0) {
        L.state = ps5lm::ServerState::loading;
        L.load_progress = g_loading;
        L.free_gib = 11.03 - g_loading * (L.model_gib + L.kv_gib);
        L.gen_tps = 0;
        L.gpu_busy = 0;
    }
    return L;
}

}  // namespace

int main(int argc, char ** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <kit assets dir> <output dir>\n", argv[0]);
        return 2;
    }
    const std::string assets = argv[1], out = argv[2];
    const int W = 1920, H = 1080;
    if (!open_context()) {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    hui::gfx::Renderer renderer;
    hui::gfx::Font regular, semibold, display, mono, pixel, hand;
    hui::ui::Fonts fonts;
    if (!renderer.init() || !load_font(renderer, assets + "/fonts/inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, assets + "/fonts/inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, assets + "/fonts/montserrat-medium.huifont", &display, &fonts.display) ||
        !load_font(renderer, assets + "/fonts/dejavu-sans-mono.huifont", &mono, &fonts.mono)) {
        return 1;
    }
    fonts.pixel = fonts.mono;
    fonts.hand  = fonts.regular;

    GLuint fb = 0, color = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);

    hui::save::ensure_directory(out);
    ps5lm::Dashboard dash(fonts);
    ps5lm::Settings settings;
    settings.default_model = "/data/PS5LM/models/Qwen3.8-27B-UD-Q2_K_XL.gguf";
    dash.set_settings(settings);
    std::vector<std::string> app_log, llama_log;
    for (int i = 0; i < 60; ++i) {
        app_log.push_back("[ps5-present-perf] calls=2500 failures=0 warmup_frames=30 idle_ms=0.000898 flip_ms=0.000889");
        app_log.push_back("ps5lm-app: model Qwen3.8-27B-UD-IQ2_XXS.gguf (qwen35): 64k context, q8_0 cache: 9.9 of 11.0 GiB");
        llama_log.push_back("7.15.902.312 I slot print_timing: id  0 | task 646 | n_gen =    162, tg =  20.44 t/s");
    }
    app_log.push_back("ps5lm-app: accept failed, errno 53");
    app_log.push_back("W srv          stop: cancel task, id_task = 22182");
    dash.set_logs(app_log, llama_log);
    std::vector<unsigned char> pixels((size_t) W * H * 4);
    stbi_flip_vertically_on_write(1);
    hui::ui::Feedback feedback;
    float t = 0;
    bool ok = true;

    const auto step = [&](int frames, hui::InputFrame input, bool generating) {
        for (int i = 0; i < frames; ++i) {
            dash.set_live(sample(t, generating));
            feedback.clear();
            dash.update(i == 0 ? input : hui::InputFrame{}, 1.0f / 60.0f, feedback);
            t += 1.0f / 60.0f;
        }
    };
    const auto shot = [&](const char * name) {
        ps5lm::DashboardFrame frame;
        frame.glass_texture = renderer.glass_texture();
        dash.draw(frame);
        renderer.begin();
        renderer.backdrop(frame.backdrop);
        renderer.draw(frame.scene);
        if (frame.glass) {
            renderer.glass();
        }
        renderer.draw(frame.overlay);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(fb, W, H);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (size_t i = 3; i < pixels.size(); i += 4) {
            pixels[i] = 255;
        }
        const std::string path = out + "/" + name + ".png";
        ok = stbi_write_png(path.c_str(), W, H, 4, pixels.data(), W * 4) != 0 && ok;
        std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls, GL error 0x%x\n", path.c_str(),
                     renderer.last_instances(), renderer.last_draw_calls(), glGetError());
    };
    auto press = [](hui::Action a) {
        hui::InputFrame in;
        in.connected = true;
        in.pressed = hui::action_bit(a);
        return in;
    };
    auto nav = [](hui::Direction d) {
        hui::InputFrame in;
        in.connected = true;
        in.nav = d;
        return in;
    };

    g_loading = 0.45f;
    step(90, {}, false);
    shot("00-loading");
    g_loading = -1;
    step(120, {}, true);
    shot("01-dashboard");
    // To the Model tile: right twice along the top, then down.
    step(20, nav(hui::Direction::right), true);
    step(20, nav(hui::Direction::right), true);
    step(40, nav(hui::Direction::down), false);
    shot("02-model-focus");
    step(60, press(hui::Action::confirm), false);
    shot("03-details");
    step(60, press(hui::Action::north), false);
    shot("03-library");
    step(30, nav(hui::Direction::up), false);
    shot("04-library-choose");
    for (int i = 0; i < 3; ++i) {
        step(25, nav(hui::Direction::down), false);
    }
    shot("05-library-usb");
    step(30, press(hui::Action::back), false);
    step(40, press(hui::Action::page_next), false);
    shot("06-settings");
    step(20, press(hui::Action::confirm), false);
    step(30, nav(hui::Direction::down), false);
    shot("07-settings-row");
    step(20, press(hui::Action::back), false);
    step(20, nav(hui::Direction::down), false);
    step(30, press(hui::Action::confirm), false);
    shot("08-settings-model");
    step(20, press(hui::Action::back), false);
    step(20, nav(hui::Direction::down), false);
    step(20, press(hui::Action::confirm), false);
    step(20, nav(hui::Direction::down), false);
    step(30, nav(hui::Direction::down), false);
    shot("08-settings-chat");
    step(40, press(hui::Action::page_next), false);
    shot("09-logs");
    step(30, press(hui::Action::north), false);
    shot("10-logs-errors");
    return ok ? 0 : 1;
}
