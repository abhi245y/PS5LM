// SPDX-License-Identifier: GPL-3.0-or-later
#include "fetch.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

extern "C" {
int sceNetInit(void);
int sceNetPoolCreate(const char * name, int size, int flags);
int sceSslInit(size_t pool);
int sceHttp2Init(int net_pool, int ssl_ctx, size_t pool, int max_requests);
int sceHttp2CreateTemplate(int ctx, const char * agent, int http_version, int auto_proxy);
int sceHttp2CreateRequestWithURL(int tmpl, const char * method, const char * url, uint64_t content_length);
int sceHttp2AddRequestHeader(int req, const char * name, const char * value, uint32_t mode);
int sceHttp2SendRequest(int req, const void * body, size_t size);
int sceHttp2GetStatusCode(int req, int * status);
int sceHttp2GetResponseContentLength(int req, int * result, uint64_t * length);
int sceHttp2ReadData(int req, void * buf, size_t size);
int sceHttp2DeleteRequest(int req);
}

namespace ps5lm {

namespace {

std::mutex g_init;
int        g_template = -1;
std::string g_init_error;

// The libraries once per process; their contexts live as long as it does.
bool init(std::string * error) {
    std::lock_guard<std::mutex> lock(g_init);
    if (g_template >= 0) {
        return true;
    }
    if (!g_init_error.empty()) {
        *error = g_init_error;
        return false;
    }
    char text[96];
    int net = -1, ssl = -1, http = -1;
    if (int rc = sceNetInit(); rc < 0) {
        std::snprintf(text, sizeof(text), "sceNetInit 0x%x", rc);
    } else if ((net = sceNetPoolCreate("ps5lm-fetch", 128 * 1024, 0)) < 0) {
        std::snprintf(text, sizeof(text), "sceNetPoolCreate 0x%x", net);
    } else if ((ssl = sceSslInit(512 * 1024)) < 0) {
        std::snprintf(text, sizeof(text), "sceSslInit 0x%x", ssl);
    } else if ((http = sceHttp2Init(net, ssl, 512 * 1024, 4)) < 0) {
        std::snprintf(text, sizeof(text), "sceHttp2Init 0x%x", http);
    } else if ((g_template = sceHttp2CreateTemplate(http, "PS5LM/1.0", 3, 1)) < 0) {
        std::snprintf(text, sizeof(text), "sceHttp2CreateTemplate 0x%x", g_template);
    } else {
        return true;
    }
    g_template = -1;
    g_init_error = text;
    *error = text;
    return false;
}

}  // namespace

FetchResult https_get(const std::string & url, const std::string & range,
                      const std::function<bool(const char *, size_t)> & sink) {
    FetchResult r;
    const auto t0 = std::chrono::steady_clock::now();
    if (!init(&r.error)) {
        return r;
    }
    char text[96];
    const int req = sceHttp2CreateRequestWithURL(g_template, "GET", url.c_str(), 0);
    if (req < 0) {
        std::snprintf(text, sizeof(text), "sceHttp2CreateRequestWithURL 0x%x", req);
        r.error = text;
        return r;
    }
    if (!range.empty()) {
        sceHttp2AddRequestHeader(req, "Range", range.c_str(), 0);
    }
    if (int rc = sceHttp2SendRequest(req, nullptr, 0); rc < 0) {
        std::snprintf(text, sizeof(text), "sceHttp2SendRequest 0x%x", rc);
        r.error = text;
        sceHttp2DeleteRequest(req);
        return r;
    }
    sceHttp2GetStatusCode(req, &r.status);
    int      has_length = 0;
    uint64_t length = 0;
    if (sceHttp2GetResponseContentLength(req, &has_length, &length) == 0 && has_length == 0) {
        r.length = (int64_t) length;  // result 0: the length is known
    }
    std::vector<char> buf(1u << 20);
    for (int n; (n = sceHttp2ReadData(req, buf.data(), buf.size())) > 0;) {
        r.bytes += (uint64_t) n;
        if (!sink(buf.data(), (size_t) n)) {
            break;
        }
    }
    sceHttp2DeleteRequest(req);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

}  // namespace ps5lm
