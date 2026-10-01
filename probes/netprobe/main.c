/* PS5LM netprobe: can a payload reach the internet to download models?
 *
 * For a few hosts: resolve the name, then open a TCP connection to port 443
 * (and 80), with timings. Separates "the DNS server does not answer for this
 * name" from "no route out". Writes to stdout and /data/PS5LM/netprobe.txt.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *g_log;

static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

/* Non-blocking connect with a 5 s timeout. */
static int try_connect(const struct in_addr *ip, int port) {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -errno;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr = *ip;
    int rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = {5, 0};
        rc = select(s + 1, NULL, &w, NULL, &tv);
        if (rc == 1) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
            rc = err ? -err : 0;
        } else {
            rc = rc == 0 ? -ETIMEDOUT : -errno;
        }
    } else if (rc != 0) {
        rc = -errno;
    }
    close(s);
    return rc;
}

int main(void) {
    static const char *hosts[] = {"huggingface.co", "us.aws.cdn.hf.co", "github.com", "example.com"};

    mkdir("/data/PS5LM", 0777);
    g_log = fopen("/data/PS5LM/netprobe.txt", "w");
    out("PS5LM netprobe\n");

    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        const double t0 = now_ms();
        const int rc = getaddrinfo(hosts[i], "443", &hints, &res);
        const double dt = now_ms() - t0;
        if (rc != 0 || !res) {
            out("%-18s dns failed (%d) after %.0f ms\n", hosts[i], rc, dt);
            continue;
        }
        const struct in_addr ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip, text, sizeof(text));
        freeaddrinfo(res);

        const double t1 = now_ms();
        const int c443 = try_connect(&ip, 443);
        const double d443 = now_ms() - t1;
        const int c80 = try_connect(&ip, 80);
        out("%-18s %-15s dns %.0f ms, tcp 443 %s (%.0f ms), tcp 80 %s\n", hosts[i], text, dt,
            c443 == 0 ? "ok" : strerror(-c443), d443, c80 == 0 ? "ok" : strerror(-c80));
    }

    out("done\n");
    if (g_log)
        fclose(g_log);
    return 0;
}
