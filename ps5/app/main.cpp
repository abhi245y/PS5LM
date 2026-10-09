// PS5LM native app: llama-server on the PS5's GPU through RADV (ggml-vulkan).
//
// A title gets the console's direct memory (12 GiB) where a payload gets none,
// and RADV turns it into Vulkan device memory, so the whole model sits on the
// GPU. The app runs llama-server on port 8081, logs to /data/PS5LM/app.log and
// takes its arguments from /data/PS5LM/app-args.txt, one per line (FTP lets
// them change without a rebuild).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "ggml-backend.h"

int llama_server(int argc, char ** argv);

// No Vulkan loader on the console: RADV is linked in whole, and ggml-vulkan's
// dispatcher starts from vkGetInstanceProcAddr, which the ICD entry point
// answers for every command.
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char * name);
extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char * name) {
    return vk_icdGetInstanceProcAddr(instance, name);
}

// The few commands ggml-vulkan calls by name rather than through its
// dispatcher go to Mesa's common implementations, which dispatch through the
// object's own table.
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
}

// A system notification on the TV: the app draws nothing itself.
static void notify(const char * text) {
    struct {
        uint8_t reserved[45];
        char    message[3075];
    } request = {};
    std::snprintf(request.message, sizeof(request.message), "%s", text);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

static const char * k_log  = "/data/PS5LM/app.log";
static const char * k_args = "/data/PS5LM/app-args.txt";

static std::vector<std::string> read_args() {
    std::vector<std::string> args;
    std::ifstream in(k_args);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
            args.push_back(line);
        }
    }
    if (args.empty()) {
        args = { "-m", "/data/PS5LM/models/Qwen3.8-27B-UD-IQ2_XXS.gguf", "-ngl", "999", "-fit", "off",
                 "-lm", "none", "-c", "4096", "-b", "256", "-ub", "64", "-np", "1",
                 "--host", "0.0.0.0", "--port", "8081" };
    }
    return args;
}

int main(int, char **) {
    // The splash stays up until a title hides it; this one draws nothing, so
    // it hides it at once (as the boilerplate's headless examples do).
    (void)sceSystemServiceHideSplashScreen();

    // stdout and stderr to a file the PC can fetch over FTP.
    if (std::freopen(k_log, "w", stdout) != nullptr) {
        setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    if (std::freopen(k_log, "a", stderr) != nullptr) {
        setvbuf(stderr, nullptr, _IONBF, 0);
    }

    // RADV splits an APU's memory into 2/3 device-local and 1/3 host heaps;
    // one device-local heap over the whole pool lets the model take nearly all
    // of the 12 GiB.
    setenv("radv_enable_unified_heap_on_apu", "true", 1);
    // A title has no HOME, and llama.cpp throws when it looks for its cache
    // directory without one.
    setenv("HOME", "/data/PS5LM", 0);
    setenv("LLAMA_CACHE", "/data/PS5LM/cache", 0);

    std::printf("ps5lm-app: start\n");
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        size_t free = 0, total = 0;
        ggml_backend_dev_memory(dev, &free, &total);
        std::printf("ps5lm-app: device %zu %s (%s): %.2f / %.2f GiB free\n", i, ggml_backend_dev_name(dev),
                    ggml_backend_dev_description(dev), free / 1073741824.0, total / 1073741824.0);
    }

    std::vector<std::string> args = read_args();
    // llama.cpp's own log, written as it goes: it survives a crash, where
    // its log thread's buffer would not.
    args.insert(args.begin(), { "--log-file", "/data/PS5LM/llama.log" });
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>("llama-server"));
    for (auto & a : args) {
        std::printf("ps5lm-app: arg %s\n", a.c_str());
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    notify("PS5LM: loading the model onto the GPU, chat on port 8081 when ready");
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
    notify(rc == 0 ? "PS5LM: server stopped" : "PS5LM: server failed, see /data/PS5LM/llama.log");
    // A title must not exit on its own (exit() and _Exit end in SIGSYS); it
    // waits for the shell to close it.
    for (;;) {
        sceKernelUsleep(2000000);
    }
}
