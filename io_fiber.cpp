#include "io_fiber.h"
#include "fiber.h"
#include "logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdint>

namespace fiber {

static bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

template<typename Func>
std::optional<ssize_t> IO::doIO(int fd, IOEvent event, Func&& op, int64_t timeout_ms) {
    auto current_fiber = Fiber::GetCurrentFiberPtr();
    if (!current_fiber) {
        LOG_ERROR("IO operations must be called from within a fiber");
        return std::nullopt;
    }
    
    auto& io_manager = IOManager::getInstance();
    auto& timer_wheel = TimerWheel::getInstance();
    
    auto timeout_state = std::make_shared<std::atomic<bool>>(false);
    auto woken_state = std::make_shared<std::atomic<bool>>(false);
    
    TimerWheel::TimerPtr timer;
    // timeout_ms == -1 means infinite wait (no timer)
    if (timeout_ms > 0) {
        timer = timer_wheel.addTimer(static_cast<uint64_t>(timeout_ms),
            [timeout_state, woken_state, &io_manager, fd, event]() {
                timeout_state->store(true, std::memory_order_release);
                if (!woken_state->exchange(true, std::memory_order_acq_rel)) {
                    io_manager.cancelEvent(fd, event);
                }
            }, false);
    }
    
    while (true) {
        ssize_t result = op();

        // success or failed for more data, complete this IO
        if (result >= 0 || errno != EAGAIN && errno != EWOULDBLOCK) {
            if (timer && !woken_state->exchange(true, std::memory_order_acq_rel)) {
                timer_wheel.cancel(timer);
            }
            return result;
        }
        
        if (timeout_state->load(std::memory_order_acquire)) {
            errno = ETIMEDOUT;
            return std::nullopt;
        }
        
        if (!io_manager.addEvent(fd, event, current_fiber)) {
            if (timer && !woken_state->exchange(true, std::memory_order_acq_rel)) {
                timer_wheel.cancel(timer);
            }
            return std::nullopt;
        }
        
        Fiber::block_yield();
        
        io_manager.delEvent(fd, event);
        
        if (timeout_state->load(std::memory_order_acquire)) {
            errno = ETIMEDOUT;
            return std::nullopt;
        }
    }
}

std::optional<ssize_t> IO::read(int fd, void* buffer, size_t len, int64_t timeout_ms) {
    setNonBlocking(fd);
    return doIO(fd, IOEvent::READ, [fd, buffer, len]() {
        return ::read(fd, buffer, len);
    }, timeout_ms);
}

std::optional<ssize_t> IO::write(int fd, const void* buffer, size_t len, int64_t timeout_ms) {
    setNonBlocking(fd);
    return doIO(fd, IOEvent::WRITE, [fd, buffer, len]() {
        return ::write(fd, buffer, len);
    }, timeout_ms);
}

std::optional<int> IO::accept(int sockfd, sockaddr* addr, socklen_t* addrlen, int64_t timeout_ms) {
    setNonBlocking(sockfd);
    auto result = doIO(sockfd, IOEvent::READ, [sockfd, addr, addrlen]() -> ssize_t {
        return ::accept(sockfd, addr, addrlen);
    }, timeout_ms);
    
    if (result) {
        int client_fd = static_cast<int>(*result);
        setNonBlocking(client_fd);  // 新连接的fd也设置为非阻塞
        return client_fd;
    }
    return std::nullopt;
}

bool IO::connect(int sockfd, const sockaddr* addr, socklen_t addrlen, int64_t timeout_ms) {
    setNonBlocking(sockfd);
    
    int ret = ::connect(sockfd, addr, addrlen);
    if (ret == 0) {
        return true;
    }
    
    if (errno != EINPROGRESS) {
        return false;
    }
    
    // 等待连接建立（WRITE事件表示连接完成）
    auto result = doIO(sockfd, IOEvent::WRITE, []() -> ssize_t {
        return 0;  // connect不需要重试，只等待一次事件
    }, timeout_ms);
    
    if (!result) {
        return false;
    }
    
    // 检查连接是否成功
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        return false;
    }
    
    if (error != 0) {
        errno = error;
        return false;
    }
    
    return true;
}} // namespace fiber
