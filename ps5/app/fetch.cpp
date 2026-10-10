// SPDX-License-Identifier: GPL-3.0-or-later
#include "fetch.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <strings.h>
#include <unistd.h>
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
int sceHttp2SetAutoRedirect(int id, int enable);
int sceHttp2GetAllResponseHeaders(int req, char ** headers, size_t * size);
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

namespace {

// The Location header of a redirect, or empty.
std::string location(int req) {
    char *  headers = nullptr;
    size_t  size = 0;
    if (sceHttp2GetAllResponseHeaders(req, &headers, &size) != 0 || !headers) {
        return {};
    }
    const std::string h(headers, size);
    for (size_t at = 0; at < h.size();) {
        const size_t end = h.find('\n', at);
        std::string line = h.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? h.size() : end + 1;
        if (line.size() > 9 && strncasecmp(line.c_str(), "location:", 9) == 0) {
            line = line.substr(9);
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of("\r\n \t") + 1);
            return line;
        }
    }
    return {};
}

}  // namespace

FetchResult https_get(const std::string & url_in, const std::string & range,
                      const std::function<bool(const char *, size_t)> & sink) {
    FetchResult r;
    const auto t0 = std::chrono::steady_clock::now();
    if (!init(&r.error)) {
        return r;
    }
    char text[160];
    // Redirects are followed here, not by the library: after following one
    // itself (Hugging Face's file links go to a CDN) it reported the length
    // but handed over no body.
    std::string url = url_in;
    int req = -1;
    for (int hop = 0; hop < 6; ++hop) {
        req = sceHttp2CreateRequestWithURL(g_template, "GET", url.c_str(), 0);
        if (req < 0) {
            std::snprintf(text, sizeof(text), "sceHttp2CreateRequestWithURL 0x%x", req);
            r.error = text;
            return r;
        }
        sceHttp2SetAutoRedirect(req, 0);
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
        if (r.status < 300 || r.status >= 400) {
            break;
        }
        const std::string next = location(req);
        sceHttp2DeleteRequest(req);
        req = -1;
        if (next.empty()) {
            r.error = "redirect without a Location";
            return r;
        }
        // A relative Location keeps the scheme and host.
        url = next.rfind("http", 0) == 0 ? next : url.substr(0, url.find('/', url.find("//") + 2)) + next;
    }
    if (req < 0) {
        r.error = "too many redirects";
        return r;
    }
    int      has_length = 0;
    uint64_t length = 0;
    if (sceHttp2GetResponseContentLength(req, &has_length, &length) == 0 && has_length == 0) {
        r.length = (int64_t) length;  // result 0: the length is known
    }
    // Large reads: 64 KiB ones ran at about 200 KB/s.
    std::vector<char> buf(4u << 20);
    for (;;) {
        const int n = sceHttp2ReadData(req, buf.data(), buf.size());
        if (n < 0) {
            std::snprintf(text, sizeof(text), "sceHttp2ReadData 0x%x after %llu bytes", n, (unsigned long long) r.bytes);
            r.error = text;
            break;
        }
        if (n == 0) {
            break;
        }
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
