// SPDX-License-Identifier: GPL-3.0-or-later
#include "market.hpp"

#include "fetch.hpp"
#include "model_plan.hpp"

#include <nlohmann/json.hpp>

extern "C" {
#include "hash/sha256/sha256.h"
}

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace ps5lm {

namespace {

constexpr const char * kHub = "https://huggingface.co";

std::string url_encode(const std::string & s) {
    std::string out;
    char hex[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char) c;
        } else {
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

// A whole response as a string, or empty with `error` set.
std::string get_text(const std::string & url, const std::string & range, std::string * error, size_t limit = 64u << 20) {
    std::string body;
    const FetchResult r = https_get(url, range, [&](const char * p, size_t n) {
        body.append(p, n);
        return body.size() < limit;
    });
    if (r.status < 200 || r.status >= 300) {
        char text[160];
        std::snprintf(text, sizeof(text), "HTTP %d%s%s", r.status, r.error.empty() ? "" : ": ", r.error.c_str());
        *error = text;
        return {};
    }
    return body;
}

}  // namespace

void Market::start(double budget_gib) {
    budget_gib_ = budget_gib;
}

MarketView Market::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    MarketView v = view_;
    v.busy = browsing_;
    return v;
}

void Market::run(std::atomic<bool> & flag, std::function<void()> job) {
    if (flag.exchange(true)) {
        return;  // one at a time; the screen shows it is busy
    }
    std::thread([&flag, job = std::move(job)] {
        job();
        flag = false;
    }).detach();
}

void Market::search(const std::string & query) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.query = query;
        view_.status = "Searching";
    }
    run(browsing_, [this, query] {
        std::string error;
        const std::string body = get_text(std::string(kHub) + "/api/models?filter=gguf&sort=downloads&direction=-1&limit=30" +
                                              (query.empty() ? "" : "&search=" + url_encode(query)),
                                          "", &error);
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        std::vector<MarketRepo> repos;
        if (j.is_array()) {
            for (const auto & m : j) {
                MarketRepo r;
                r.id = m.value("id", std::string());
                r.downloads = m.value("downloads", (uint64_t) 0);
                if (!r.id.empty()) {
                    repos.push_back(r);
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (view_.query != query) {
            return;  // a newer search replaced this one
        }
        view_.repos = repos;
        view_.open = -1;
        view_.status = !error.empty() ? error : repos.empty() ? "Nothing found" : "";
    });
}

void Market::open_repo(int index) {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || index >= (int) view_.repos.size()) {
            return;
        }
        id = view_.repos[(size_t) index].id;
        view_.open = index;
        view_.status = "Listing files";
    }
    run(browsing_, [this, index, id] {
        std::string error;
        const std::string body = get_text(std::string(kHub) + "/api/models/" + id + "/tree/main", "", &error);
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        std::vector<MarketFile> files;
        if (j.is_array()) {
            for (const auto & e : j) {
                const std::string path = e.value("path", std::string());
                const bool gguf = path.size() > 5 && path.compare(path.size() - 5, 5, ".gguf") == 0;
                // Projectors and later parts of split models are not models of their own.
                if (!gguf || path.find("mmproj") != std::string::npos ||
                    (path.find("-of-") != std::string::npos && path.find("-00001-of-") == std::string::npos)) {
                    continue;
                }
                MarketFile f;
                f.name = path;
                f.bytes = e.value("size", (uint64_t) 0);
                if (e.contains("lfs") && e["lfs"].is_object()) {
                    f.sha256 = e["lfs"].value("oid", std::string());
                    f.bytes = e["lfs"].value("size", f.bytes);
                }
                // Before the header is read: fits if the weights and the
                // planner's smallest reserve do.
                f.fits = f.bytes / 1073741824.0 + 1.0 <= budget_gib_ ? -1 : 0;
                if (f.fits == 0) {
                    f.plan = "too big: the weights alone need more than is free";
                }
                files.push_back(f);
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (view_.open == index && index < (int) view_.repos.size()) {
            view_.repos[(size_t) index].files = files;
        }
        view_.status = !error.empty() ? error : files.empty() ? "No GGUF files in this repository" : "";
    });
}

void Market::plan_file(int repo, int file) {
    std::string id, name;
    uint64_t    bytes = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (repo < 0 || repo >= (int) view_.repos.size() || file < 0 ||
            file >= (int) view_.repos[(size_t) repo].files.size()) {
            return;
        }
        const MarketFile & f = view_.repos[(size_t) repo].files[(size_t) file];
        if (f.fits != -1) {
            return;  // planned already, or too big by its size alone
        }
        id = view_.repos[(size_t) repo].id;
        name = f.name;
        bytes = f.bytes;
    }
    run(browsing_, [this, repo, file, id, name, bytes] {
        std::string error;
        const std::string head = get_text(std::string(kHub) + "/" + id + "/resolve/main/" + name, "bytes=0-16777215", &error,
                                          16u << 20);
        const ModelInfo m = read_model_header(name, head, bytes);
        const Plan      p = plan_model(m, budget_gib_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (repo < (int) view_.repos.size() && file < (int) view_.repos[(size_t) repo].files.size()) {
            MarketFile & f = view_.repos[(size_t) repo].files[(size_t) file];
            f.fits = m.ok ? (p.fits ? 1 : 0) : -2;
            f.plan = m.ok ? p.why : (!error.empty() ? error : m.error);
        }
    });
}

void Market::download(int repo, int file, const std::string & dir) {
    std::string id, name, sha;
    uint64_t    bytes = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (repo < 0 || repo >= (int) view_.repos.size() || file < 0 ||
            file >= (int) view_.repos[(size_t) repo].files.size()) {
            return;
        }
        const MarketFile & f = view_.repos[(size_t) repo].files[(size_t) file];
        id = view_.repos[(size_t) repo].id;
        name = f.name;
        sha = f.sha256;
        bytes = f.bytes;
        if (downloading_) {
            return;
        }
        view_.download_file = name;
        view_.download_progress = 0;
        view_.download_result.clear();
    }
    run(downloading_, [this, id, name, sha, bytes, dir] {
        const size_t slash = name.find_last_of('/');
        const std::string to = dir + "/" + name.substr(slash == std::string::npos ? 0 : slash + 1);
        const std::string part = to + ".part";
        // write(), not stdio: on /data stdio managed 18 MB/s, write() 230 MB/s.
        const int out = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        sha256_t h;
        sha256_init(&h);
        uint64_t done = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const FetchResult r = https_get(std::string(kHub) + "/" + id + "/resolve/main/" + name, "", [&](const char * p, size_t n) {
            if (out < 0 || write(out, p, n) != (ssize_t) n) {
                return false;
            }
            sha256_update(&h, (const unsigned char *) p, n);
            done += n;
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::lock_guard<std::mutex> lock(mutex_);
            view_.download_progress = bytes ? (float) ((double) done / (double) bytes) : 0.0f;
            view_.download_mbps = s > 0 ? done / 1e6 / s : 0;
            return true;
        });
        bool ok = out >= 0 && fsync(out) == 0;
        if (out >= 0) {
            close(out);
        }
        unsigned char digest[32];
        sha256_final(&h, digest);
        char hex[65];
        for (int i = 0; i < 32; ++i) {
            std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        }
        std::string result;
        if (!ok || r.status != 200 || done != bytes) {
            char text[160];
            std::snprintf(text, sizeof(text), "Failed: HTTP %d, %llu of %llu bytes%s%s", r.status,
                          (unsigned long long) done, (unsigned long long) bytes, r.error.empty() ? "" : ", ",
                          r.error.c_str());
            result = text;
        } else if (!sha.empty() && sha != hex) {
            result = "Failed: the checksum does not match";
        } else if (std::rename(part.c_str(), to.c_str()) != 0) {
            result = "Failed: could not rename the file";
        } else {
            result = "Saved to " + to;
        }
        if (result.rfind("Saved", 0) != 0) {
            unlink(part.c_str());
        }
        std::printf("ps5lm-app: download %s/%s: %s\n", id.c_str(), name.c_str(), result.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        view_.download_progress = -1;
        view_.download_result = result;
    });
}

}  // namespace ps5lm
