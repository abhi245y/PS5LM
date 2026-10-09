/* libc functions llama.cpp's common code references that no system module
 * gives a title. None of them is on the inference path: they back spawning
 * tools, the home directory lookup, cookie dates and mmap hints.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>

int execlp(const char *file, const char *arg, ...) {
    (void)file;
    (void)arg;
    errno = ENOSYS;
    return -1;
}

int pipe2(int fds[2], int flags) {
    if (pipe(fds) != 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        if ((flags & O_CLOEXEC) && fcntl(fds[i], F_SETFD, FD_CLOEXEC) == -1)
            goto fail;
        if ((flags & O_NONBLOCK) && fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK) == -1)
            goto fail;
    }
    return 0;
fail:
    close(fds[0]);
    close(fds[1]);
    return -1;
}

struct passwd *getpwuid(uid_t uid) {
    (void)uid;
    return NULL;
}

/* Days from 1970-01-01 to the given civil date (Howard Hinnant's algorithm). */
static long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

time_t timegm(struct tm *tm) {
    long long y = tm->tm_year + 1900LL;
    long long mon = tm->tm_mon;
    y += mon / 12;
    mon %= 12;
    if (mon < 0) {
        mon += 12;
        y--;
    }
    const long long days = days_from_civil(y, (unsigned)mon + 1, 1) + tm->tm_mday - 1;
    return (time_t)(days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
}

int posix_madvise(void *addr, size_t len, int advice) {
    (void)addr;
    (void)len;
    (void)advice;
    return 0;
}

/* Imports only libScePosixForWebKit or libkernel_sys define in the SDK's
 * stubs. Those modules give a title nothing, so each import would be a call
 * through NULL (isatty, from llama.cpp's log setup, was the first). Defined
 * here, the link binds them locally. */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

int isatty(int fd) {
    (void)fd;
    errno = ENOTTY;
    return 0;
}

pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

long pathconf(const char *path, int name) {
    (void)path;
    (void)name;
    errno = EINVAL;
    return -1;
}

ssize_t readlink(const char *restrict path, char *restrict buf, size_t size) {
    (void)path;
    (void)buf;
    (void)size;
    errno = EINVAL; /* not a symbolic link */
    return -1;
}

int link(const char *from, const char *to) {
    (void)from;
    (void)to;
    errno = ENOSYS;
    return -1;
}

int symlink(const char *from, const char *to) {
    (void)from;
    (void)to;
    errno = ENOSYS;
    return -1;
}

int mkstemp(char *path) {
    const size_t n = strlen(path);
    if (n < 6 || strcmp(path + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static unsigned counter;
    for (int attempt = 0; attempt < 100; attempt++) {
        unsigned v = (unsigned)time(NULL) * 2654435761u + (++counter) * 40503u + (unsigned)attempt;
        for (int i = 0; i < 6; i++) {
            path[n - 6 + i] = chars[v % 36];
            v = v / 36 + 7919u * (unsigned)i;
        }
        const int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    errno = EEXIST;
    return -1;
}

/* Numeric only: the console has no reverse lookup, and the HTTP server asks
 * for numeric host and port. */
int getnameinfo(const struct sockaddr *restrict sa, socklen_t salen, char *restrict host, size_t hostlen,
                char *restrict serv, size_t servlen, int flags) {
    (void)salen;
    (void)flags;
    const void *addr;
    unsigned port;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        addr = &in->sin_addr;
        port = ntohs(in->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        addr = &in6->sin6_addr;
        port = ntohs(in6->sin6_port);
    } else {
        return EAI_FAMILY;
    }
    if (host != NULL && hostlen > 0 && inet_ntop(sa->sa_family, addr, host, (socklen_t)hostlen) == NULL)
        return EAI_OVERFLOW;
    if (serv != NULL && servlen > 0 && snprintf(serv, servlen, "%u", port) >= (int)servlen)
        return EAI_OVERFLOW;
    return 0;
}

/* The SDK fork's platform getaddrinfo always fails (EAI_FAIL), and
 * cpp-httplib resolves even the address it binds to through getaddrinfo:
 * llama-server could not bind 0.0.0.0. Numeric addresses and the passive
 * wildcard are all a server needs; names are refused. */
#include <stdlib.h>

static struct addrinfo *make_ai(int family, int socktype, int protocol, const void *addr, unsigned port) {
    const size_t addrlen = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    struct addrinfo *ai = calloc(1, sizeof(*ai) + addrlen);
    if (ai == NULL)
        return NULL;
    ai->ai_family = family;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = (socklen_t)addrlen;
    ai->ai_addr = (struct sockaddr *)(ai + 1);
    if (family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)ai->ai_addr;
        in->sin_len = sizeof(*in);
        in->sin_family = AF_INET;
        in->sin_port = htons((unsigned short)port);
        memcpy(&in->sin_addr, addr, sizeof(in->sin_addr));
    } else {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ai->ai_addr;
        in6->sin6_len = sizeof(*in6);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons((unsigned short)port);
        memcpy(&in6->sin6_addr, addr, sizeof(in6->sin6_addr));
    }
    return ai;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    const int family = hints ? hints->ai_family : AF_UNSPEC;
    const int socktype = hints ? hints->ai_socktype : 0;
    const int protocol = hints ? hints->ai_protocol : 0;
    const int passive = hints && (hints->ai_flags & AI_PASSIVE);
    unsigned port = 0;
    if (service != NULL && *service != '\0') {
        char *end;
        const unsigned long v = strtoul(service, &end, 10);
        if (*end != '\0' || v > 65535)
            return EAI_SERVICE;
        port = (unsigned)v;
    }
    struct in_addr v4;
    struct in6_addr v6;
    if (node == NULL || strcmp(node, "localhost") == 0) {
        if (family == AF_INET6) {
            v6 = node == NULL && passive ? in6addr_any : in6addr_loopback;
            *res = make_ai(AF_INET6, socktype, protocol, &v6, port);
        } else {
            v4.s_addr = htonl(node == NULL && passive ? INADDR_ANY : INADDR_LOOPBACK);
            *res = make_ai(AF_INET, socktype, protocol, &v4, port);
        }
    } else if (family != AF_INET6 && inet_pton(AF_INET, node, &v4) == 1) {
        *res = make_ai(AF_INET, socktype, protocol, &v4, port);
    } else if (family != AF_INET && inet_pton(AF_INET6, node, &v6) == 1) {
        *res = make_ai(AF_INET6, socktype, protocol, &v6, port);
    } else {
        return EAI_NONAME;
    }
    return *res != NULL ? 0 : EAI_MEMORY;
}

void freeaddrinfo(struct addrinfo *ai) {
    while (ai != NULL) {
        struct addrinfo *next = ai->ai_next;
        free(ai);
        ai = next;
    }
}

/* accept4 for the title, with the reason logged when accept fails: the HTTP
 * listener stops on any error it does not know to be transient. */
int accept4(int s, struct sockaddr *restrict addr, socklen_t *restrict addrlen, int flags) {
    const int fd = accept(s, addr, addrlen);
    if (fd < 0) {
        const int saved = errno;
        fprintf(stderr, "ps5lm-app: accept failed, errno %d\n", saved);
        errno = saved;
        return fd;
    }
    if (flags & SOCK_NONBLOCK) {
        const int fl = fcntl(fd, F_GETFL);
        if (fl != -1)
            (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    return fd;
}
