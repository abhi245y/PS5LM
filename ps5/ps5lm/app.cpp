// PS5LM: the payload people load from Payload Manager.
//
// One process, two servers. The library (port 8082) lists models that fit a
// payload, downloads them from Hugging Face over HTTPS (mbedTLS, with the
// Mozilla CA list built in) and picks the one to run. The chat server is
// llama.cpp's own llama-server, started inside this process on port 8081 with
// the chosen model; switching models stops it with llama_server_terminate()
// and starts it again. On start, the PS5's browser opens the library. A
// watchdog rebuilds the servers after rest mode or a network change, when
// their sockets go stale.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>
#include "ps5lm_assets.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int  llama_server(int argc, char ** argv);
void llama_server_terminate();

extern "C" {
int sceUserServiceInitialize(void * params);
int sceSystemServiceLaunchWebBrowser(const char * uri, void * param);
typedef struct {
    char unused[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int device, notify_request_t * req, size_t size, int blocking);
int sceShellCoreUtilResetAutoPowerDownTimer(void);
int sceKernelAvailableFlexibleMemorySize(size_t * size);
}

using json = nlohmann::json;

namespace {

const std::string kRoot     = "/data/PS5LM";
const std::string kModels   = kRoot + "/models";
const std::string kWww      = kRoot + "/www";
const std::string kCaFile   = kRoot + "/cacert.pem";
const std::string kLastFile = kRoot + "/last_model.txt";
const std::string kLogFile  = kRoot + "/ps5lm.log";
const int kLibraryPort = 8082;
const int kChatPort    = 8081;
const char * kVersion  = "0.1.0";

// Before this, the console's clock is wrong and every certificate looks not
// yet valid: Payload Manager users hit it as TLS failures (its #23, #45, #54).
constexpr time_t kReleaseEpoch = 1790812800;  // 2026-10-01

// statfs on /data reports more free space than Settings does (PS5 Game
// Compressor #43), so a download keeps this much in reserve.
constexpr long long kSpareBytes = 512LL * 1024 * 1024;

// The model is locked in memory (see run_loop), and locked pages are taken
// from the same pool the home screen and the browser need. 1.3 GB locked runs
// fine; 2.7 GB froze the console. Bigger models wait for a native app (#4).
constexpr long long kMaxModelBytes = 1600LL * 1024 * 1024;

// Models measured on a PS5 Slim. Sizes are the exact Hugging Face file sizes.
struct CatalogModel {
    const char * id;
    const char * name;
    const char * repo;
    const char * file;
    long long    size;
    const char * note;
};

const CatalogModel kCatalog[] = {
    {"qwen3.5-0.8b", "Qwen3.5 0.8B", "unsloth/Qwen3.5-0.8B-GGUF", "Qwen3.5-0.8B-Q4_K_M.gguf", 532517120LL,
     "Fastest: 13 to 21 tokens/s on a PS5 Slim."},
    {"qwen3.5-2b", "Qwen3.5 2B", "unsloth/Qwen3.5-2B-GGUF", "Qwen3.5-2B-Q4_K_M.gguf", 1280835840LL,
     "Better answers, about 9 tokens/s on a PS5 Slim."},
};

const CatalogModel * find_catalog(const std::string & id) {
    for (const auto & m : kCatalog) {
        if (id == m.id) {
            return &m;
        }
    }
    return nullptr;
}

// ---- small helpers -------------------------------------------------------

long long file_size(const std::string & path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long long) st.st_size : -1;
}

long long free_bytes(const std::string & path) {
    struct statfs fs;
    return statfs(path.c_str(), &fs) == 0 ? (long long) fs.f_bavail * (long long) fs.f_bsize : -1;
}

bool write_file(const std::string & path, const unsigned char * data, size_t len) {
    FILE * f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

std::string read_text(const std::string & path) {
    std::string out;
    FILE * f = fopen(path.c_str(), "rb");
    if (!f) {
        return out;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    fclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

// A file name inside the models folder: no paths, a .gguf.
bool valid_model_name(const std::string & name) {
    return !name.empty() && name.find('/') == std::string::npos && name.find("..") == std::string::npos &&
           name.size() > 5 && name.compare(name.size() - 5, 5, ".gguf") == 0;
}

std::vector<std::pair<std::string, long long>> installed_models() {
    std::vector<std::pair<std::string, long long>> out;
    DIR * dir = opendir(kModels.c_str());
    if (!dir) {
        return out;
    }
    while (dirent * e = readdir(dir)) {
        const std::string name = e->d_name;
        if (valid_model_name(name)) {
            out.emplace_back(name, file_size(kModels + "/" + name));
        }
    }
    closedir(dir);
    return out;
}

// The console's address on the network, for the notification.
std::string local_ip() {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return "";
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port   = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &to.sin_addr);
    std::string ip;
    if (connect(s, (sockaddr *) &to, sizeof(to)) == 0) {
        sockaddr_in me{};
        socklen_t   len = sizeof(me);
        if (getsockname(s, (sockaddr *) &me, &len) == 0) {
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &me.sin_addr, text, sizeof(text));
            ip = text;
        }
    }
    close(s);
    return ip;
}

void notify(const std::string & text) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", text.c_str());
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// Runs fn on a thread with a big stack: mbedTLS and the JSON code want more
// than the platform default.
void spawn(std::function<void()> fn) {
    auto * heap = new std::function<void()>(std::move(fn));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &attr, [](void * arg) -> void * {
        std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(arg));
        (*f)();
        return nullptr;
    }, heap);
    pthread_attr_destroy(&attr);
}

// ---- downloads -------------------------------------------------------------

struct Download {
    long long   done    = 0;
    long long   total   = 0;
    double      speed   = 0;  // bytes per second
    bool        active  = false;
    bool        cancel  = false;
    std::string error;
};

std::mutex                      g_dl_mu;
std::map<std::string, Download> g_downloads;

struct Url {
    std::string origin;  // https://host[:port]
    std::string path;    // /path?query
};

bool split_url(const std::string & url, Url & out) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) {
        return false;
    }
    const auto slash = url.find('/', scheme + 3);
    out.origin = url.substr(0, slash);
    out.path   = slash == std::string::npos ? "/" : url.substr(slash);
    return true;
}

// The console's kernel caps a socket's receive buffer at 64 KB, so one
// connection to the CDN (~150 ms away) tops out near 0.9 MB/s. A download is
// split into 8 MiB pieces fetched over kConnections connections at once, the
// way hf_transfer does it. Pieces land in place in <file>.part, and
// <file>.pieces records the finished ones, so an interrupted download only
// fetches what is missing.
constexpr int       kConnections = 16;
constexpr long long kPiece       = 8LL * 1024 * 1024;

httplib::Client make_client(const std::string & origin) {
    httplib::Client cli(origin);
    cli.set_ca_cert_path(kCaFile);
    cli.enable_server_certificate_verification(true);
    cli.set_follow_location(false);
    cli.set_keep_alive(true);
    cli.set_connection_timeout(20);
    cli.set_read_timeout(60);
    return cli;
}

// Follows the Hugging Face redirect to the CDN by hand (each hop gets the CA
// list) and returns the signed CDN URL, or "" with why set.
std::string resolve(const CatalogModel & m, std::string & why) {
    std::string url = std::string("https://huggingface.co/") + m.repo + "/resolve/main/" + m.file;
    for (int hop = 0; hop < 6; hop++) {
        Url u;
        if (!split_url(url, u)) {
            why = "bad URL: " + url;
            return "";
        }
        auto cli = make_client(u.origin);
        httplib::Headers headers = {{"User-Agent", std::string("ps5lm/") + kVersion}, {"Range", "bytes=0-0"}};
        auto res = cli.Get(u.path, headers);
        if (!res) {
            why = "network error: " + httplib::to_string(res.error());
            if (res.error() == httplib::Error::SSLServerVerification && time(nullptr) < kReleaseEpoch) {
                why = "the console's date and time are wrong, set them in Settings";
            }
            return "";
        }
        if (res->status >= 300 && res->status < 400) {
            std::string next = res->get_header_value("Location");
            url = next.rfind("http", 0) == 0 ? next : u.origin + next;
            continue;
        }
        if (res->status == 200 || res->status == 206) {
            return url;
        }
        why = "server answered " + std::to_string(res->status);
        return "";
    }
    why = "too many redirects";
    return "";
}

void set_download(const CatalogModel & m, const std::function<void(Download &)> & fn) {
    std::lock_guard<std::mutex> lock(g_dl_mu);
    fn(g_downloads[m.id]);
}

bool download_cancelled(const CatalogModel & m) {
    std::lock_guard<std::mutex> lock(g_dl_mu);
    return g_downloads[m.id].cancel;
}

// A failed piece goes back in the queue, and its connection waits 1, 2, 4, 8
// then 16 s before trying again; it gives up after kMaxStreak failures in a
// row. A short Wi-Fi drop costs a pause, not the download.
constexpr int kMaxStreak = 8;

void download(const CatalogModel & m) {
    const std::string final_path  = kModels + "/" + m.file;
    const std::string part_path   = final_path + ".part";
    const std::string pieces_path = final_path + ".pieces";
    const long long   total       = m.size;
    const int         count       = (int) ((total + kPiece - 1) / kPiece);

    auto fail = [&](const std::string & why) {
        set_download(m, [&](Download & d) {
            d.active = false;
            d.error  = why;
        });
        fprintf(stderr, "[ps5lm] download %s: %s\n", m.id, why.c_str());
    };

    // Which pieces are already on disk.
    std::vector<char> done(count, '0');
    {
        FILE * f = fopen(pieces_path.c_str(), "rb");
        if (f) {
            if (fread(done.data(), 1, count, f) != (size_t) count) {
                std::fill(done.begin(), done.end(), '0');
            }
            fclose(f);
        }
        if (file_size(part_path) != total) {
            std::fill(done.begin(), done.end(), '0');
        }
    }
    long long have = 0;
    for (int i = 0; i < count; i++) {
        if (done[i] == '1') {
            have += std::min(kPiece, total - (long long) i * kPiece);
        }
    }
    if (free_bytes(kRoot) >= 0 && free_bytes(kRoot) < total - have + kSpareBytes) {
        fail("not enough free space on the console");
        return;
    }

    const int fd = open(part_path.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd < 0 || ftruncate(fd, total) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        fail("could not create " + part_path);
        return;
    }
    const int pieces_fd = open(pieces_path.c_str(), O_RDWR | O_CREAT, 0666);
    if (pieces_fd < 0 || pwrite(pieces_fd, done.data(), count, 0) != count) {
        close(fd);
        if (pieces_fd >= 0) {
            close(pieces_fd);
        }
        fail("could not create " + pieces_path);
        return;
    }

    std::string why;
    std::string cdn = resolve(m, why);
    if (cdn.empty()) {
        close(fd);
        close(pieces_fd);
        fail(why);
        return;
    }

    std::mutex              mu;  // guards cdn, next, done
    int                     next = 0;
    std::atomic<long long>  received{have};
    std::atomic<bool>       disk_full{false};
    std::atomic<int>        running{0};
    std::mutex              finish_mu;
    std::condition_variable finish_cv;

    set_download(m, [&](Download & d) {
        d.done  = have;
        d.total = total;
    });

    // The record marks only pieces whose data has reached the disk: it is
    // written from a copy taken before the fsync, so a crash or a power cut
    // never leaves a piece marked done that holds zeros. PS5 Game Compressor
    // syncs before it renames for the same reason.
    auto save_record = [&] {
        std::vector<char> copy;
        {
            std::lock_guard<std::mutex> lock(mu);
            copy = done;
        }
        fsync(fd);
        pwrite(pieces_fd, copy.data(), count, 0);
    };

    auto worker = [&] {
        std::string origin;
        std::unique_ptr<httplib::Client> cli;
        int streak = 0;  // failures in a row on this connection
        for (;;) {
            int         piece = -1;
            std::string url;
            {
                std::lock_guard<std::mutex> lock(mu);
                while (next < count && done[next] == '1') {
                    next++;
                }
                if (next < count) {
                    piece = next++;
                    url   = cdn;
                }
            }
            if (piece < 0 || disk_full || download_cancelled(m)) {
                break;
            }

            Url u;
            split_url(url, u);
            if (!cli || origin != u.origin) {
                origin = u.origin;
                cli.reset(new httplib::Client(make_client(origin)));
            }
            const long long start = (long long) piece * kPiece;
            const long long end   = std::min(total, start + kPiece) - 1;
            long long       wrote = 0;
            httplib::Headers headers = {
                {"User-Agent", std::string("ps5lm/") + kVersion},
                {"Range", "bytes=" + std::to_string(start) + "-" + std::to_string(end)},
            };
            int  status = 0;
            auto res    = cli->Get(u.path, headers,
                [&](const httplib::Response & r) {
                    status = r.status;
                    return r.status == 206;
                },
                [&](const char * data, size_t len) {
                    if (pwrite(fd, data, len, start + wrote) != (ssize_t) len) {
                        if (errno == ENOSPC) {
                            disk_full = true;
                        }
                        return false;
                    }
                    wrote += (long long) len;
                    received += (long long) len;
                    return !download_cancelled(m);
                });

            if (res && status == 206 && wrote == end - start + 1) {
                streak = 0;
                std::lock_guard<std::mutex> lock(mu);
                done[piece] = '1';
                continue;
            }
            received -= wrote;  // the piece will be fetched again
            {
                std::lock_guard<std::mutex> lock(mu);
                if (piece < next) {
                    next = piece;  // retry it
                }
            }
            cli.reset();
            if (disk_full || ++streak >= kMaxStreak) {
                break;
            }
            if (status == 403 || status == 410) {
                // The signed CDN link expired: get a fresh one, outside the
                // lock so the other connections keep going.
                std::string again_why;
                const std::string again = resolve(m, again_why);
                if (!again.empty()) {
                    std::lock_guard<std::mutex> lock(mu);
                    cdn = again;
                }
            }
            const int wait_ms = 1000 << std::min(streak - 1, 4);
            for (int ms = 0; ms < wait_ms && !download_cancelled(m); ms += 250) {
                usleep(250 * 1000);
            }
        }
        std::lock_guard<std::mutex> lock(finish_mu);
        running--;
        finish_cv.notify_all();
    };

    running = kConnections;
    for (int i = 0; i < kConnections; i++) {
        spawn(worker);
    }

    // Report progress until every worker is done. Every few seconds, save the
    // record, and keep the console from going into rest mode on its own timer
    // (PS5 Game Compressor resets the same timer while it works).
    long long last  = received;
    auto      t0    = std::chrono::steady_clock::now();
    auto      saved = t0;
    auto      awake = t0 - std::chrono::seconds(10);
    {
        std::unique_lock<std::mutex> lock(finish_mu);
        while (running > 0) {
            finish_cv.wait_for(lock, std::chrono::seconds(1));
            const auto   now = std::chrono::steady_clock::now();
            const double dt  = std::chrono::duration<double>(now - t0).count();
            const long long got = received;
            set_download(m, [&](Download & d) {
                d.done = got;
                if (dt >= 1.0) {
                    d.speed = (double) (got - last) / dt;
                }
            });
            if (dt >= 1.0) {
                last = got;
                t0   = now;
            }
            if (now - awake >= std::chrono::seconds(10)) {
                sceShellCoreUtilResetAutoPowerDownTimer();
                awake = now;
            }
            if (now - saved >= std::chrono::seconds(5)) {
                save_record();
                saved = now;
            }
        }
    }
    save_record();
    close(fd);
    close(pieces_fd);

    if (download_cancelled(m)) {
        set_download(m, [&](Download & d) {
            d.active = false;
            d.error  = "cancelled";
        });
        return;
    }
    if (disk_full) {
        fail("the console's storage is full");
        return;
    }
    for (int i = 0; i < count; i++) {
        if (done[i] != '1') {
            fail("some pieces failed, press Resume to fetch the rest");
            return;
        }
    }
    if (rename(part_path.c_str(), final_path.c_str()) != 0) {
        fail("could not rename " + part_path + ": " + strerror(errno));
        return;
    }
    unlink(pieces_path.c_str());
    set_download(m, [&](Download & d) {
        d.active = false;
        d.done   = total;
        d.error.clear();
    });
    fprintf(stderr, "[ps5lm] downloaded %s\n", m.file);
}

// ---- the chat server -------------------------------------------------------

std::mutex              g_run_mu;
std::condition_variable g_run_cv;
std::string             g_wanted;   // model file to start next
std::string             g_running;  // model file being served
std::string             g_run_error;

void request_model(const std::string & file) {
    std::lock_guard<std::mutex> lock(g_run_mu);
    g_wanted = file;
    if (!g_running.empty()) {
        llama_server_terminate();
    }
    g_run_cv.notify_all();
}

void run_loop() {
    for (;;) {
        std::string file;
        {
            std::unique_lock<std::mutex> lock(g_run_mu);
            g_run_cv.wait(lock, [] { return !g_wanted.empty(); });
            file = g_wanted;
            g_wanted.clear();
            g_running = file;
            g_run_error.clear();
        }
        write_file(kLastFile, (const unsigned char *) file.data(), file.size());

        const std::string model = kModels + "/" + file;
        const std::string port  = std::to_string(kChatPort);
        // Read the model into memory and lock it there (-lm mlock): the PS5
        // pages a payload's memory out to disk once the browser in front needs
        // room, and an unlocked model then crawls. A 4096 token context fits.
        std::vector<std::string> args = {
            "llama-server", "-m", model, "-c", "4096", "-np", "1", "-lm", "mlock",
            "--path", kWww, "--host", "0.0.0.0", "--port", port,
        };
        std::vector<char *> argv;
        for (auto & a : args) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);

        fprintf(stderr, "[ps5lm] starting llama-server with %s\n", file.c_str());
        const int rc = llama_server((int) args.size(), argv.data());
        fprintf(stderr, "[ps5lm] llama-server returned %d\n", rc);

        std::lock_guard<std::mutex> lock(g_run_mu);
        if (rc != 0 && g_wanted.empty()) {
            g_run_error = "the model did not start (see " + kLogFile + ")";
        }
        g_running.clear();
    }
}

bool chat_ready() {
    httplib::Client cli("127.0.0.1", kChatPort);
    cli.set_connection_timeout(0, 300000);
    cli.set_read_timeout(1, 0);
    auto res = cli.Get("/health");
    return res && res->status == 200;
}

// Whether llama-server answers at all: its /health says 503 while a model
// loads, and the HTTP side starts before the model does.
bool chat_answers() {
    httplib::Client cli("127.0.0.1", kChatPort);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(2, 0);
    return (bool) cli.Get("/health");
}

// ---- the library server ----------------------------------------------------

// How much of an interrupted download is on disk, from its pieces record.
long long paused_bytes(const CatalogModel & m) {
    const std::string pieces = read_text(kModels + "/" + m.file + ".pieces");
    long long got = 0;
    for (size_t i = 0; i < pieces.size(); i++) {
        if (pieces[i] == '1') {
            got += std::min(kPiece, m.size - (long long) i * kPiece);
        }
    }
    return got;
}

json state_json() {
    json catalog = json::array();
    {
        std::lock_guard<std::mutex> lock(g_dl_mu);
        for (const auto & m : kCatalog) {
            json item = {
                {"id", m.id}, {"name", m.name}, {"file", m.file}, {"size", m.size}, {"note", m.note},
                {"installed", file_size(kModels + "/" + m.file) == m.size},
            };
            auto it = g_downloads.find(m.id);
            if (it != g_downloads.end()) {
                item["download"] = {
                    {"active", it->second.active}, {"done", it->second.done}, {"total", it->second.total},
                    {"speed", it->second.speed}, {"error", it->second.error},
                };
            } else if (long long got = paused_bytes(m); got > 0) {
                item["download"] = {
                    {"active", false}, {"done", got}, {"total", m.size}, {"speed", 0}, {"error", "paused"},
                };
            }
            catalog.push_back(item);
        }
    }

    json installed = json::array();
    for (const auto & [name, size] : installed_models()) {
        installed.push_back({{"file", name}, {"size", size}});
    }

    json chat;
    {
        std::lock_guard<std::mutex> lock(g_run_mu);
        chat = {{"model", g_running.empty() ? g_wanted : g_running}, {"error", g_run_error}};
    }
    chat["ready"] = !chat["model"].get<std::string>().empty() && chat_ready();
    chat["port"]  = kChatPort;

    return {
        {"version", kVersion}, {"catalog", catalog}, {"installed", installed}, {"chat", chat},
        {"free", free_bytes(kRoot)}, {"ip", local_ip()}, {"port", kLibraryPort}, {"max_model", kMaxModelBytes},
    };
}

std::atomic<bool> g_quit{false};     // SIGTERM or the Quit button
std::atomic<bool> g_resumed{false};  // SIGCONT: the console woke from rest mode

// Leaves without Sony's exit(), which would run the static destructors while
// the server and download threads still use those objects (compat/exit.cpp,
// issue #3). An interrupted download resumes from its record next time.
[[noreturn]] void quit_now() {
    fprintf(stderr, "[ps5lm] quitting\n");
    notify("PS5LM stopped");
    fflush(nullptr);
    _Exit(0);
}

std::unique_ptr<httplib::Server> make_library() {
    auto server = std::make_unique<httplib::Server>();
    httplib::Server & svr = *server;

    // SO_REUSEADDR only. httplib's default is SO_REUSEPORT, which lets a
    // second copy bind the same port, and then the "already running" check in
    // main never fires. ftpsrv, websrv and Payload Manager all set REUSEADDR.
    svr.set_socket_options([](socket_t s) {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    });
    // The PS5 browser can hold on to a cached page even after the cache is
    // cleared in Settings (Payload Manager #40, fixed there in 3efc4f4).
    svr.set_post_routing_handler([](const httplib::Request &, httplib::Response & res) {
        res.set_header("Cache-Control", "no-store");
    });

    svr.Get("/", [](const httplib::Request &, httplib::Response & res) {
        res.set_content((const char *) library_html, library_html_len, "text/html; charset=utf-8");
    });

    svr.Get("/api/state", [](const httplib::Request &, httplib::Response & res) {
        res.set_content(state_json().dump(), "application/json");
    });

    // For the watchdog: answers as long as the server does.
    svr.Get("/api/ping", [](const httplib::Request &, httplib::Response & res) {
        res.set_content("ok", "text/plain");
    });

    svr.Post("/api/download", [](const httplib::Request & req, httplib::Response & res) {
        const CatalogModel * m = find_catalog(req.get_param_value("id"));
        if (!m) {
            res.status = 404;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_dl_mu);
            auto & d = g_downloads[m->id];
            if (d.active) {
                return;
            }
            d = Download{};
            d.active = true;
            d.total  = m->size;
        }
        spawn([m] { download(*m); });
    });

    svr.Post("/api/cancel", [](const httplib::Request & req, httplib::Response &) {
        std::lock_guard<std::mutex> lock(g_dl_mu);
        auto it = g_downloads.find(req.get_param_value("id"));
        if (it != g_downloads.end()) {
            it->second.cancel = true;
        }
    });

    svr.Post("/api/run", [](const httplib::Request & req, httplib::Response & res) {
        const std::string file = req.get_param_value("file");
        if (!valid_model_name(file) || file_size(kModels + "/" + file) <= 0) {
            res.status = 404;
            return;
        }
        if (file_size(kModels + "/" + file) > kMaxModelBytes) {
            std::lock_guard<std::mutex> lock(g_run_mu);
            g_run_error = file + " is too big for a payload: the limit is 1.6 GB";
            res.status  = 413;
            return;
        }
        request_model(file);
    });

    svr.Post("/api/stop", [](const httplib::Request &, httplib::Response &) {
        std::lock_guard<std::mutex> lock(g_run_mu);
        g_wanted.clear();
        if (!g_running.empty()) {
            llama_server_terminate();
        }
    });

    svr.Post("/api/delete", [](const httplib::Request & req, httplib::Response & res) {
        const std::string file = req.get_param_value("file");
        if (!valid_model_name(file)) {
            res.status = 404;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_run_mu);
            if (g_running == file || g_wanted == file) {
                res.status = 409;
                return;
            }
        }
        unlink((kModels + "/" + file).c_str());
        unlink((kModels + "/" + file + ".part").c_str());
        unlink((kModels + "/" + file + ".pieces").c_str());
    });

    // Payload Manager can only SIGKILL a payload; this frees the model's
    // locked memory from the page itself. The watchdog does the leaving, once
    // this answer is out.
    svr.Post("/api/quit", [](const httplib::Request &, httplib::Response &) {
        g_quit = true;
    });

    return server;
}

std::mutex        g_svr_mu;
httplib::Server * g_svr = nullptr;  // the library server listening now

// Serves the library for as long as the process runs. After rest mode the
// listening socket can go stale without an error, and on some network errors
// httplib returns from listen; either way a new server is built and bound
// again (Payload Manager #61 and cd9ad1f, websrv e5a6369, ftpsrv 4f7408a).
void serve_library(httplib::Server * first) {
    std::unique_ptr<httplib::Server> svr(first);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_svr_mu);
            g_svr = svr.get();
        }
        svr->listen_after_bind();
        {
            std::lock_guard<std::mutex> lock(g_svr_mu);
            g_svr = nullptr;
        }
        fprintf(stderr, "[ps5lm] library server stopped, starting it again\n");
        do {
            sleep(1);
            svr = make_library();
        } while (!svr->bind_to_port("0.0.0.0", kLibraryPort));
    }
}

void restart_library() {
    std::lock_guard<std::mutex> lock(g_svr_mu);
    if (g_svr) {
        g_svr->stop();
    }
}

bool library_answers() {
    httplib::Client cli("127.0.0.1", kLibraryPort);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(2, 0);
    auto res = cli.Get("/api/ping");
    return res && res->status == 200;
}

// Restarts the servers when they may have gone deaf: on waking from rest mode
// (SIGCONT, as Payload Manager does it), on a new address, or when the library
// misses two checks in a row on loopback. Also logs free flexible memory when
// it moves by 64 MiB or more, for issue #10 (PS5CEMU-HAR logs its own the same
// way).
void watchdog() {
    std::string ip          = local_ip();
    int         misses      = 0;
    size_t      logged_free = 0;
    for (int tick = 1;; tick++) {
        sleep(1);
        if (g_quit) {
            quit_now();
        }
        const bool resumed = g_resumed.exchange(false);
        if (!resumed && tick % 5 != 0) {
            continue;
        }

        const std::string now_ip = local_ip();
        if (resumed || now_ip != ip) {
            fprintf(stderr, "[ps5lm] %s, restarting the servers\n",
                    resumed ? "woke from rest mode" : ("address " + ip + " -> " + now_ip).c_str());
            ip = now_ip;
            restart_library();
            std::string running;
            {
                std::lock_guard<std::mutex> lock(g_run_mu);
                running = g_running;
            }
            sleep(1);
            if (!running.empty() && !chat_answers()) {
                request_model(running);
            }
            misses = 0;
            continue;
        }

        misses = library_answers() ? 0 : misses + 1;
        if (misses >= 2) {
            fprintf(stderr, "[ps5lm] the library stopped answering, restarting it\n");
            restart_library();
            misses = 0;
        }

        size_t free_flex = 0;
        if (sceKernelAvailableFlexibleMemorySize(&free_flex) == 0 &&
            (free_flex >= logged_free + (64u << 20) || free_flex + (64u << 20) <= logged_free)) {
            fprintf(stderr, "[ps5lm] free flexible memory: %zu MiB\n", free_flex >> 20);
            logged_free = free_flex;
        }
    }
}

void open_browser(const std::string & url) {
    const int rc = sceSystemServiceLaunchWebBrowser(url.c_str(), nullptr);
    if (rc != 0) {
        fprintf(stderr, "[ps5lm] could not open the browser: 0x%08x\n", (unsigned) rc);
        notify("Open " + url + " in the browser");
    }
}

}  // namespace

int main(int argc, char ** argv) {
    // The name Payload Manager looks payloads up by (ftpsrv, websrv and
    // Payload Manager name themselves the same way).
    syscall(SYS_thr_set_name, -1, "ps5lm.elf");
    signal(SIGPIPE, SIG_IGN);
    sceUserServiceInitialize(nullptr);

    mkdir(kRoot.c_str(), 0777);
    mkdir(kModels.c_str(), 0777);
    mkdir(kWww.c_str(), 0777);

    const std::string library_url = "http://127.0.0.1:" + std::to_string(kLibraryPort) + "/";

    // A second copy finds the port taken: show the running one and leave.
    auto library = make_library();
    if (!library->bind_to_port("0.0.0.0", kLibraryPort)) {
        notify("PS5LM is already running");
        open_browser(library_url);
        fflush(nullptr);
        _Exit(0);
    }

    // Started from Payload Manager there is nowhere to print; keep a log.
    // `ps5lm.elf --console` (from a shell) prints instead.
    if (!(argc > 1 && strcmp(argv[1], "--console") == 0)) {
        freopen(kLogFile.c_str(), "w", stderr);
        dup2(fileno(stderr), fileno(stdout));
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    fprintf(stderr, "[ps5lm] PS5LM %s, library on port %d\n", kVersion, kLibraryPort);

    signal(SIGCONT, [](int) { g_resumed = true; });
    signal(SIGTERM, [](int) { g_quit = true; });

    write_file(kWww + "/index.html", chat_html, chat_html_len);
    write_file(kCaFile, ca_bundle, ca_bundle_len);

    httplib::Server * first = library.release();
    spawn([first] { serve_library(first); });
    spawn(watchdog);

    // Pick up where the last session left off.
    const std::string last = read_text(kLastFile);
    if (valid_model_name(last) && file_size(kModels + "/" + last) > 0 &&
        file_size(kModels + "/" + last) <= kMaxModelBytes) {
        request_model(last);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const std::string ip = local_ip();
    notify("PS5LM is running\nOn your phone or computer: http://" + (ip.empty() ? std::string("<console IP>") : ip) +
           ":" + std::to_string(kLibraryPort));
    open_browser(library_url);

    run_loop();
    return 0;
}
