// SPDX-License-Identifier: GPL-3.0-or-later
#include "stats.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef __PROSPERO__
#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>
extern "C" int scePthreadGetaffinity(void * thread, uint64_t * mask);
extern "C" void * scePthreadSelf(void);
#endif

namespace ps5lm {

namespace {

constexpr size_t kHistory = 120;  // seconds of history in the charts
constexpr double kGiB     = 1073741824.0;

void push(std::vector<float> & v, float x) {
    v.push_back(x);
    if (v.size() > kHistory) {
        v.erase(v.begin());
    }
}

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double cpu_seconds() {
    rusage u = {};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

int usable_cpus() {
#ifdef __PROSPERO__
    // The CPUs the title may run on (the app is not given all sixteen).
    uint64_t mask = 0;
    if (scePthreadGetaffinity(scePthreadSelf(), &mask) == 0 && mask != 0) {
        return __builtin_popcountll(mask);
    }
#endif
    const unsigned n = std::thread::hardware_concurrency();
    return n ? (int) n : 1;
}

// A JSON integer after "key": in the first slot of /slots (one slot runs).
long json_int(const std::string & text, const char * key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = text.find(k);
    return at == std::string::npos ? 0 : std::strtol(text.c_str() + at + k.size(), nullptr, 10);
}

}  // namespace

std::string http_get(int port, const char * path, int timeout_s) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }
    timeval tv = { timeout_s, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr = {};
#ifdef __FreeBSD__
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t) port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string out;
    if (connect(fd, (sockaddr *) &addr, sizeof(addr)) == 0) {
        char req[160];
        const int n = std::snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: localhost\r\n\r\n", path);
        send(fd, req, (size_t) n, 0);
        char buf[4096];
        for (ssize_t got; (got = recv(fd, buf, sizeof(buf), 0)) > 0;) {
            out.append(buf, (size_t) got);
            if (out.size() > (1u << 20)) {
                break;
            }
        }
    }
    close(fd);
    const size_t body = out.find("\r\n\r\n");
    return body == std::string::npos ? std::string() : out.substr(body + 4);
}

double metric(const std::string & text, const char * name) {
    const std::string key = std::string("\nllamacpp:") + name + " ";
    const size_t at = text.find(key);
    return at == std::string::npos ? 0.0 : std::strtod(text.c_str() + at + key.size(), nullptr);
}

std::string local_address() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return {};
    }
    sockaddr_in to = {};
#ifdef __FreeBSD__
    to.sin_len = sizeof(to);
#endif
    to.sin_family = AF_INET;
    to.sin_port   = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &to.sin_addr);
    std::string ip;
    if (connect(fd, (sockaddr *) &to, sizeof(to)) == 0) {
        sockaddr_in me = {};
        socklen_t   len = sizeof(me);
        char        buf[INET_ADDRSTRLEN] = {};
        if (getsockname(fd, (sockaddr *) &me, &len) == 0 && inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf))) {
            ip = buf;
        }
    }
    close(fd);
    return ip == "0.0.0.0" ? std::string() : ip;
}

void StatsCollector::start(int port) {
    port_ = port;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        live_.cpus = usable_cpus();
        const std::string ip = local_address();
        live_.address = "http://" + (ip.empty() ? std::string("<console IP>") : ip) + ":" + std::to_string(port);
    }
    std::thread([this] { run(); }).detach();
}

Live StatsCollector::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_;
}

void StatsCollector::set_model(const Live & m) {
    std::lock_guard<std::mutex> lock(mutex_);
    live_.model_label = m.model_label;
    live_.model_arch  = m.model_arch;
    live_.preset      = m.preset;
    live_.model_gib   = m.model_gib;
    live_.ctx         = m.ctx;
    live_.kv_type     = m.kv_type;
    live_.kv_gib      = m.kv_gib;
}

void StatsCollector::set_models(const std::vector<ModelRow> & rows) {
    std::lock_guard<std::mutex> lock(mutex_);
    live_.models = rows;
}

void StatsCollector::set_state(ServerState s) {
    std::lock_guard<std::mutex> lock(mutex_);
    live_.state = s;
    // Loading starts from what the pool holds now; progress is measured against
    // what the model and its cache will add.
    load_base_ = s == ServerState::loading ? live_.pool_gib - live_.free_gib : -1;
    live_.load_progress = s == ServerState::loading ? 0.0f : -1.0f;
}

void StatsCollector::run() {
    const double start = now_s();
    double last_wall = start, last_cpu = cpu_seconds();
    double last_busy = 0, last_tokens = 0, last_gen_s = 0;
    bool   have_last = false, last_working = false;
    double last_ptok = 0, last_psec = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const double wall = now_s(), cpu = cpu_seconds();

        const std::string m = http_get(port_, "/metrics");
        const bool        up = !m.empty();
        const double busy_s = metric(m, "prompt_seconds_total") + metric(m, "tokens_predicted_seconds_total");
        const double tokens = metric(m, "tokens_predicted_total");
        const double gen_s  = metric(m, "tokens_predicted_seconds_total");
        const bool   working = metric(m, "requests_processing") > 0;
        std::string  slots;
        if (up) {
            slots = http_get(port_, "/slots");
        }

        double pool = 0, free = 0, heap = 0;
#ifdef __PROSPERO__
        const int64_t pool_bytes = sceKernelGetDirectMemorySize();
        int64_t       at = 0;
        size_t        avail = 0;
        sceKernelAvailableDirectMemorySize(0, pool_bytes, 2 * 1024 * 1024, &at, &avail);
        struct ps5_heap_stats hs = {};
        ps5_heap_stats(&hs);
        pool = pool_bytes / kGiB;
        free = avail / kGiB;
        heap = hs.mapped_bytes / kGiB;
#endif

        std::lock_guard<std::mutex> lock(mutex_);
        const double dt = wall - last_wall;
        live_.uptime_s = wall - start;
        live_.pool_gib = pool;
        live_.free_gib = free;
        live_.heap_gib = heap;
        live_.cpu_use  = (float) std::min(1.0, (cpu - last_cpu) / (dt * live_.cpus));
        if (live_.state == ServerState::loading && load_base_ >= 0 && live_.model_gib + live_.kv_gib > 0) {
            const double added = (pool - free) - load_base_;
            live_.load_progress = (float) std::clamp(added / (live_.model_gib + live_.kv_gib), 0.0, 0.99);
        }
        push(live_.cpu_history, live_.cpu_use);
        if (up) {
            if (live_.state == ServerState::loading || live_.state == ServerState::ready ||
                live_.state == ServerState::generating) {
                live_.state = working ? ServerState::generating : ServerState::ready;
                live_.load_progress = -1;
            }
            live_.tokens        = (uint64_t) tokens;
            live_.prompt_tokens = (uint64_t) metric(m, "prompt_tokens_total");
            if (last_working && !working && tokens > last_tokens) {
                live_.requests++;  // a reply finished
            }
            last_working = working;
            // The last prompt's speed, from the counters (the server's own
            // average reads 0 on this build).
            const double ptok = metric(m, "prompt_tokens_total"), psec = metric(m, "prompt_seconds_total");
            if (have_last && psec - last_psec > 0.01) {
                live_.prompt_tps = (float) ((ptok - last_ptok) / (psec - last_psec));
            }
            last_ptok = ptok;
            last_psec = psec;
            if (have_last) {
                live_.gpu_busy = (float) std::min(1.0, std::max(0.0, (busy_s - last_busy) / dt));
                const double dgen = gen_s - last_gen_s;
                live_.gen_tps = dgen > 0.05 ? (float) ((tokens - last_tokens) / dgen) : 0.0f;
            }
            live_.ctx_used = (uint32_t) (json_int(slots, "n_prompt_tokens") + json_int(slots, "n_decoded"));
            have_last   = true;
            last_busy   = busy_s;
            last_tokens = tokens;
            last_gen_s  = gen_s;
        }
        push(live_.gpu_history, live_.gpu_busy);
        push(live_.gen_history, live_.gen_tps);
        last_wall = wall;
        last_cpu  = cpu;
    }
}

}  // namespace ps5lm
