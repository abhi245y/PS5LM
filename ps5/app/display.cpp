// The app's screen: an OpenGL 4.6 context on the TV through ps5-opengl's EGL,
// opened in the order ps5-homebrew-ui's display_egl.cpp uses. The GL stack is
// linked in as one object with only its API global (scripts/build-app.sh), so
// its Mesa does not meet RADV's.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "display.hpp"

#include "gfx/renderer.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#include <ps5_opengl_display.h>
#include <ps5_opengl_display_modes.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

extern "C" {
// Both GPU drivers in the title initialise AGC; the first call does it and
// every later one waits for it and gets the same answer (as ProsperoAI does
// for its OpenGL UI and its own compute). Linked with --wrap=sceAgcInit.
int __real_sceAgcInit(uint32_t flags);
int sceKernelUsleep(uint32_t microseconds);

int __wrap_sceAgcInit(uint32_t flags) {
    static std::atomic<int> state{ 0 };
    static int              result = 0;
    int                     expected = 0;
    if (state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        result = __real_sceAgcInit(flags);
        state.store(2, std::memory_order_release);
    } else {
        while (state.load(std::memory_order_acquire) != 2) {
            sceKernelUsleep(100);
        }
    }
    return result;
}

// The GL runtime's thread_local context has an initialiser function the SDK
// does not define; it has nothing to do (ProsperoAI's runtime_shims.c).
void ps5lm_glapi_tls_context_init(void) __asm__("_ZTH23_mesa_glapi_tls_Context");
void ps5lm_glapi_tls_context_init(void) {}
}

namespace ps5lm {

namespace {

constexpr EGLint kConfig[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE,
};
constexpr EGLint kContext[] = {
    EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
    EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE,
};

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;

bool fail(const char * what) {
    std::printf("ps5lm-app: display: %s failed (egl 0x%04x)\n", what, (unsigned) eglGetError());
    return false;
}

}  // namespace

bool Display::open(int width, int height) {
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) {
        return fail("eglGetDisplay");
    }
    if (!eglSetDisplayModePS5(g_display, width, height)) {
        fail("eglSetDisplayModePS5");
    }
    EGLint major = 0, minor = 0, count = 0;
    EGLConfig config = nullptr;
    if (!eglInitialize(g_display, &major, &minor)) {
        return fail("eglInitialize");
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        return fail("eglBindAPI");
    }
    if (!eglChooseConfig(g_display, kConfig, &config, 1, &count) || count != 1) {
        return fail("eglChooseConfig");
    }
    // The PS5 backend owns the window: native handle 0.
    g_surface = eglCreateWindowSurface(g_display, config, (EGLNativeWindowType) 0, nullptr);
    if (g_surface == EGL_NO_SURFACE) {
        return fail("eglCreateWindowSurface");
    }
    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, kContext);
    if (g_context == EGL_NO_CONTEXT) {
        return fail("eglCreateContext");
    }
    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        return fail("eglMakeCurrent");
    }
    eglQuerySurface(g_display, g_surface, EGL_WIDTH, &width_);
    eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &height_);
    eglSwapInterval(g_display, 1);
    std::printf("ps5lm-app: display: egl %d.%d, %dx%d, %s\n", major, minor, width_, height_,
                (const char *) glGetString(GL_RENDERER));
    return true;
}

void Display::clear(float r, float g, float b) {
    glViewport(0, 0, width_, height_);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Display::present(hui::gfx::Renderer & renderer) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    renderer.present(0, width_, height_);
}

bool Display::swap() {
    return eglSwapBuffers(g_display, g_surface) == EGL_TRUE;
}

}  // namespace ps5lm
