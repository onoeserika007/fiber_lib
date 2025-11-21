#pragma once

#include <chrono>
#include <optional>
#include <sys/socket.h>
#include <sys/types.h>
#include "io_manager.h"
#include "timer.h"

namespace fiber {

// All IO operations automatically set fd to non-blocking mode
// Default timeout is -1 (infinite wait), mimicking blocking IO semantics
// timeout_ms: timeout in milliseconds, -1 means infinite wait
class IO {
public:
    static std::optional<ssize_t> read(int fd, void *buffer, size_t len, int64_t timeout_ms = -1);

    static std::optional<ssize_t> read_et(int fd, void *buffer, size_t len, int64_t timeout_ms = -1);

    static std::optional<ssize_t> write(int fd, const void *buffer, size_t len, int64_t timeout_ms = -1);

    static std::optional<int> accept(int sockfd, sockaddr *addr, socklen_t *addrlen, int64_t timeout_ms = -1);

    static std::vector<int> accept_et(int sockfd, sockaddr *addr, socklen_t *addrlen, int64_t timeout_ms = -1);

    static bool connect(int sockfd, const sockaddr *addr, socklen_t addrlen, int64_t timeout_ms = -1);

    static std::optional<ssize_t> writev(int fd, const iovec *iov, int iovcnt, int64_t timeout_ms = -1);

    static std::optional<ssize_t> sendfile(int out_fd, int in_fd, off_t *offset, size_t count, int64_t timeout_ms = -1);

    static std::optional<ssize_t> recv(int fd, void *buffer, size_t len, int flags, int64_t timeout_ms = -1);

    static std::optional<ssize_t> recv_et(int fd, void *buffer, size_t len, int flags, int64_t timeout_ms = -1);

    // Close fd and clean up all IOManager state
    static int close(int fd);

    static int shutdown(int fd, int how);

private:
    template<typename Func>
    static std::optional<ssize_t> doIO(int fd, IOEvent event, Func &&op, int64_t timeout_ms);
};

} // namespace fiber
