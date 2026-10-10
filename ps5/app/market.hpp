// The model market: search Hugging Face for GGUF models, show which files
// fit this console, and download one into the models folder. Network work
// runs on its own thread; the screen reads a snapshot.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps5lm {

struct MarketFile {
    std::string name;            // "Qwen3.8-27B-UD-Q2_K_XL.gguf"
    std::string sha256;          // from the repository's LFS pointer
    uint64_t    bytes = 0;
    std::string plan;            // the planner's line, once its header was read
    int         fits  = -1;      // 1 fits, 0 too big, -1 not planned yet
};

struct MarketRepo {
    std::string id;              // "unsloth/Qwen3.8-27B-GGUF"
    uint64_t    downloads = 0;
    std::vector<MarketFile> files;  // filled when the repository is opened
};

struct MarketView {
    std::string query;
    bool        busy = false;    // a search, listing or plan is running
    std::string status;          // the last error or note, for the screen
    std::vector<MarketRepo> repos;
    int         open = -1;       // the repository whose files are listed
    // The download in progress, or the last one.
    std::string download_file;
    float       download_progress = -1;  // 0..1 while it runs
    double      download_mbps = 0;
    std::string download_result; // "Saved to …" or the error
};

// One file to download, by name: it survives the app restarting (a download
// asked for after unloading the model is picked up by the new process).
struct MarketJob {
    std::string repo, file, sha256, dir;
    uint64_t    bytes = 0;
};

class Market {
  public:
    // budget_gib: GPU memory free before any model, for the fit column.
    void start(double budget_gib);
    MarketView snapshot();

    void search(const std::string & query);
    void open_repo(int index);
    void plan_file(int repo, int file);  // reads its header (16 MiB range request)
    bool job(int repo, int file, const std::string & dir, MarketJob * out);
    // `free_gib`: room on the drive it goes to, or -1 when unknown (the check is skipped).
    void download(const MarketJob & job, double free_gib);

  private:
    // One browse job (search, listing, plan) and one download at a time.
    void run(std::atomic<bool> & flag, std::function<void()> job);
    std::mutex mutex_;
    MarketView view_;
    double     budget_gib_ = 0;
    std::atomic<bool> browsing_{ false };
    std::atomic<bool> downloading_{ false };
};

}  // namespace ps5lm
