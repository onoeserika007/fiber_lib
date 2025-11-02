#pragma once

#include "io_manager.h"
#include "timer.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <chrono>
#include <optional>

namespace fiber {

// All IO operations automatically set fd to non-blocking mode
// Default timeout is -1 (infinite wait), mimicking blocking IO semantics
// timeout_ms: timeout in milliseconds, -1 means infinite wait
class IO {
public:
    static std::optional<ssize_t> read(int fd, void* buffer, size_t len, int64_t timeout_ms = -1);
    
    static std::optional<ssize_t> write(int fd, const void* buffer, size_t len, int64_t timeout_ms = -1);
    
    static std::optional<int> accept(int sockfd, sockaddr* addr, socklen_t* addrlen, int64_t timeout_ms = -1);
    
    static bool connect(int sockfd, const sockaddr* addr, socklen_t addrlen, int64_t timeout_ms = -1);
    
    // Close fd and clean up all IOManager state
    static void close(int fd);

private:
    template<typename Func>
    static std::optional<ssize_t> doIO(int fd, IOEvent event, Func&& op, int64_t timeout_ms);
};

} // namespace fiber
