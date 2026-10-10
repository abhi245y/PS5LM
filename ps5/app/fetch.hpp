// HTTPS from the title through the system's own HTTP/2 and TLS libraries
// (libSceHttp2, libSceSsl), as ps5-payload-dev's http2_get sample uses them:
// no TLS library or certificates of our own.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ps5lm {

struct FetchResult {
    int         status = -1;  // HTTP status, or -1 when the request did not go out
    int64_t     length = -1;  // Content-Length, or -1
    uint64_t    bytes  = 0;   // body bytes handed to the sink
    double      seconds = 0;
    std::string error;        // the failing call and its code, when status is -1
};

// GET `url`, optionally with a Range header ("bytes=0-16777215"). Each piece
// of the body goes to `sink`; returning false from it stops the download.
FetchResult https_get(const std::string & url, const std::string & range,
                      const std::function<bool(const char *, size_t)> & sink);

}  // namespace ps5lm
