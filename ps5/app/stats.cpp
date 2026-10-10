// SPDX-License-Identifier: GPL-3.0-or-later
#include "stats.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
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
// libkernel; signatures as Marice/ps5-exporter and the SDK's hwinfo sample use them.
extern "C" int  sceKernelGetCpuTemperature(int * celsius);
extern "C" int  sceKernelGetSocSensorTemperature(int sensor, int * celsius);
extern "C" int  sceKernelGetSocPowerConsumption(uint64_t * out, double reserved);
extern "C" long sceKernelGetCpuFrequency(void);
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

// Free and total GiB of the filesystem holding `path`, or zeros.
void space(const char * path, double * free, double * total) {
    struct statvfs v = {};
    if (statvfs(path, &v) != 0 || v.f_blocks == 0) {
        *free = *total = 0;
        return;
    }
    *free  = (double) v.f_bavail * v.f_frsize / kGiB;
    *total = (double) v.f_blocks * v.f_frsize / kGiB;
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

// One sample from Prometheus text by its full name and labels
// ("ps5_temperature_celsius{sensor=\"cpu\"}"), or `missing`.
double sample(const std::string & text, const std::string & key, double missing) {
    const size_t at = text.find("\n" + key + " ");
    return at == std::string::npos ? missing : std::strtod(text.c_str() + at + key.size() + 2, nullptr);
}

// Sends a payload to an ELF loader on this console (elfldr, port 9021).
bool send_payload(const char * path, int port) {
    FILE * f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in to = {};
#ifdef __FreeBSD__
    to.sin_len = sizeof(to);
#endif
    to.sin_family = AF_INET;
    to.sin_port   = htons((uint16_t) port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = fd >= 0 && connect(fd, (sockaddr *) &to, sizeof(to)) == 0;
    char buf[65536];
    for (size_t n; ok && (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) {
        ok = send(fd, buf, n, 0) == (ssize_t) n;
    }
    std::fclose(f);
    if (fd >= 0) {
        close(fd);
    }
    return ok;
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
    live_.model_file  = m.model_file;
    live_.plan        = m.plan;
    live_.args        = m.args;
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

        float soc_t = -1000, cpu_t = -1000, watts = 0, ghz = 0;
#ifdef __PROSPERO__
        int t = 0;
        if (sceKernelGetSocSensorTemperature(0, &t) == 0 && t > -100 && t < 200) {
            soc_t = (float) t;
        }
        if (sceKernelGetCpuTemperature(&t) == 0 && t > -100 && t < 200) {
            cpu_t = (float) t;
        }
        // The unit is not documented; milliwatts is the community reading,
        // so only plausible values are shown.
        uint64_t raw[16] = {};
        if (sceKernelGetSocPowerConsumption(raw, 0.0) == 0 && raw[0] >= 1000 && raw[0] <= 350000) {
            watts = (float) (raw[0] / 1000.0);
        }
        const long hz = sceKernelGetCpuFrequency();
        ghz = hz > 0 ? (float) (hz / 1e9) : 0.0f;
        static bool logged = false;  // ponytail: once, to see what a title is allowed to read
        if (!logged) {
            logged = true;
            int t0 = -1000, t1 = -1000;
            const int r0 = sceKernelGetSocSensorTemperature(0, &t0), r1 = sceKernelGetCpuTemperature(&t1);
            std::printf("ps5lm-app: sensors: soc rc 0x%x t %d, cpu rc 0x%x t %d, power raw %llu, cpu hz %ld\n", r0, t0,
                        r1, t1, (unsigned long long) raw[0], hz);
        }
#endif
        // A title may not read the sensors (0x80020002); a payload may.
        // Marice/ps5-exporter, when loaded, serves them on port 9100.
        float fan = -1;
        bool  from_exporter = false;
        if (soc_t < -100) {
            std::string p = http_get(9100, "/metrics");
            // Not running: hand the bundled copy to elfldr, at start and then
            // once a minute (elfldr may come later).
            if (p.empty() && (long) (wall - start) % 60 == 1) {
                const bool sent = send_payload("/app0/payloads/ps5-exporter.elf", 9021);
                std::printf("ps5lm-app: ps5-exporter not running; %s\n",
                            sent ? "sent the bundled copy to elfldr" : "no elfldr on 9021 to load it");
            }
            if (!p.empty()) {
                from_exporter = true;
                soc_t = (float) sample(p, "ps5_temperature_celsius{sensor=\"soc0\"}", -1000);
                cpu_t = (float) sample(p, "ps5_temperature_celsius{sensor=\"cpu\"}", -1000);
                watts = (float) sample(p, "ps5_soc_power_watts", 0);
                fan   = (float) sample(p, "ps5_fan_duty_ratio", -1);
                if (ghz <= 0) {
                    ghz = (float) (sample(p, "ps5_cpu_frequency_hertz", 0) / 1e9);
                }
            }
        }
        double data_free = 0, data_total = 0, usb_free = 0, usb_total = 0;
        space("/data", &data_free, &data_total);
        // An empty /mnt/usb0 is a directory of the root filesystem, not a drive.
        struct statvfs usb = {}, mnt = {};
        if (statvfs("/mnt/usb0", &usb) == 0 && statvfs("/mnt", &mnt) == 0 && usb.f_fsid != mnt.f_fsid) {
            space("/mnt/usb0", &usb_free, &usb_total);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const double dt = wall - last_wall;
        live_.soc_temp = soc_t;
        live_.cpu_temp = cpu_t;
        live_.soc_power_w = watts;
        live_.cpu_ghz = ghz;
        live_.fan = fan;
        live_.sensors_from_exporter = from_exporter;
        if (soc_t > -100) {
            push(live_.temp_history, soc_t);
        }
        live_.data_free = data_free;
        live_.data_total = data_total;
        live_.usb_free = usb_free;
        live_.usb_total = usb_total;
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
