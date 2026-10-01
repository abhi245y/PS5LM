/* Functions the PS5's libc and libkernel don't export, built on ones they do.
 * scripts/build-llama.sh links this archive last into every llama.cpp tool.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

/* The SDK's headers declare accept4 (FreeBSD 10), but libkernel only exports
 * accept. cpp-httplib's server loop calls accept4(..., SOCK_CLOEXEC). */
int accept4(int s, struct sockaddr *__restrict addr, socklen_t *__restrict addrlen, int flags) {
    const int fd = accept(s, addr, addrlen);
    if (fd < 0)
        return fd;
    if ((flags & SOCK_CLOEXEC) && fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        goto fail;
    if (flags & SOCK_NONBLOCK) {
        const int fl = fcntl(fd, F_GETFL);
        if (fl == -1 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) == -1)
            goto fail;
    }
    return fd;

fail:;
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}
