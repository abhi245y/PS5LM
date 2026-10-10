// What the dashboard shows, and the thread that measures it.
//
// llama-server reports its own work through /metrics and /slots, read here
// over loopback: tokens per second from the predicted-token counters, and
// how busy the GPU is from the seconds the server spent computing prompts
// and tokens (every layer runs on the GPU, so that time is GPU time). CPU
// use comes from getrusage, memory from the kernel's direct-memory pool and
// the platform heap.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ps5lm {

enum class ServerState : uint8_t { loading, ready, generating, failed, no_model };

struct ModelRow {
    std::string file;   // path
    std::string label;  // file name without .gguf
    std::string arch;
    std::string plan;   // the planner's line ("64k context, q8_0 cache: 9.9 of 11.0 GiB")
    std::string preset;     // the preset that filled its sampling ("Qwen 3.8"), or empty
    double      size_gib = 0;
    bool        fits     = false;
    bool        current  = false;
    bool        on_usb   = false;  // on a USB drive; loads from there like internal ones
};

struct Live {
    ServerState state = ServerState::loading;
    std::string address;     // "http://192.168.1.19:8081"

    // The model that runs.
    std::string model_label;
    std::string model_arch;
    std::string preset;      // "Qwen 3.8"
    double      model_gib = 0;
    uint32_t    ctx       = 0;
    std::string kv_type;
    double      kv_gib    = 0;
    std::string model_file;              // full path
    std::string plan;                    // the planner's line
    std::vector<std::string> args;       // llama-server's arguments

    // Speed.
    float gen_tps    = 0;    // over the last generation interval
    float prompt_tps = 0;    // last prompt
    std::vector<float> gen_history;   // tok/s per second, oldest first (0 while idle)
    std::vector<float> gpu_history;   // GPU busy 0..1 per second
    std::vector<float> cpu_history;   // CPU use 0..1 per second

    // Load.
    float gpu_busy = 0;      // 0..1
    float cpu_use  = 0;      // 0..1 of the CPUs the app may use
    int   cpus     = 0;

    // The console. Temperatures in degrees C, below -100 when unread; power
    // in watts, 0 when unread; the CPU clock in GHz.
    float soc_temp = -1000;
    float cpu_temp = -1000;
    float soc_power_w = 0;
    float cpu_ghz = 0;
    float fan = -1;              // fan duty 0..1, below 0 when unread
    bool  sensors_from_exporter = false;  // read from ps5-exporter, not the kernel
    std::vector<float> temp_history;  // SoC degrees C per second
    std::vector<float> power_history; // SoC watts per second
    double energy_wh = 0;             // SoC energy since the app started
    std::vector<std::pair<std::string, float>> temps;  // every sensor that answered ("cpu", "soc0", ...)

    // Storage, GiB; usb_total 0 when no drive is mounted.
    double data_free = 0, data_total = 0;
    double usb_free  = 0, usb_total  = 0;
    double scratch_gib = 0;  // the chat tools' scratch folder

    // Memory, GiB.
    double pool_gib = 0;     // the title's direct memory
    double free_gib = 0;
    double heap_gib = 0;     // CPU heap, part of the pool

    // Loading: how much of the model and its cache is on the GPU, 0..1, or -1.
    float load_progress = -1;

    // Session.
    double   uptime_s  = 0;
    uint64_t tokens    = 0;  // generated since start
    uint64_t prompt_tokens = 0;
    uint64_t requests  = 0;
    uint32_t ctx_used  = 0;  // in the running slot

    std::vector<ModelRow> models;
};

// The sampling thread. Fill the static parts (model, address) with set_*;
// snapshot() copies everything for the frame.
class StatsCollector {
  public:
    void start(int port);
    Live snapshot();
    void set_model(const Live & model_part);   // model_* , ctx, kv_*, preset
    void set_models(const std::vector<ModelRow> & rows);
    void set_state(ServerState s);
    void set_scratch(double gib);

  private:
    void run();
    std::mutex mutex_;
    Live       live_;
    double     load_base_ = -1;   // pool in use when the load began
    int        port_ = 8081;
};

// The console's address on the LAN, as the PC sees it ("192.168.1.19"), or
// empty. Asks the routing table through an unconnected UDP socket: nothing is sent.
// Sends a payload file to an ELF loader on this console; false if none listens.
bool send_payload(const char * path, int port);

std::string local_address();

// A tiny HTTP GET over loopback; returns the body, or empty.
std::string http_get(int port, const char * path, int timeout_s = 1);

// One value from Prometheus text ("llamacpp:tokens_predicted_total 42").
double metric(const std::string & text, const char * name);

}  // namespace ps5lm
