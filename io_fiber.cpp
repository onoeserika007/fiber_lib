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

        // success or failed not for waiting more data(EAGAIN | EWOULDBLOCK) or wait for connecting, complete this IO
        if (result >= 0
            || errno != EAGAIN && errno != EWOULDBLOCK && errno != EINPROGRESS && errno != EALREADY) {
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
    // LOG_DEBUG("[fiber::IO] Calling read by fd={}", fd);
    return doIO(fd, IOEvent::READ, [fd, buffer, len]() {
        return ::read(fd, buffer, len);
    }, timeout_ms);
}

std::optional<ssize_t> IO::write(int fd, const void* buffer, size_t len, int64_t timeout_ms) {
    setNonBlocking(fd);
    // LOG_DEBUG("[fiber::IO] Calling write by fd={}", fd);
    return doIO(fd, IOEvent::WRITE, [fd, buffer, len]() {
        return ::write(fd, buffer, len);
    }, timeout_ms);
}

std::optional<int> IO::accept(int sockfd, sockaddr* addr, socklen_t* addrlen, int64_t timeout_ms) {
    setNonBlocking(sockfd);
    // LOG_DEBUG("[fiber::IO] Calling accept by fd={}", sockfd);
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
    // LOG_DEBUG("[fiber::IO] Calling connect by fd={}", sockfd);
    int ret = ::connect(sockfd, addr, addrlen);
    if (ret == 0) {
        return true;
    }
    
    if (errno != EINPROGRESS) {
        IO::close(sockfd);
        return false;
    }
    
    // 等待连接建立（WRITE事件表示连接完成）
    auto result = doIO(sockfd, IOEvent::WRITE, [sockfd]() -> ssize_t {
        // 检查连接是否建立
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
            LOG_ERROR("[IO::connect] getsockopt calling failed, unknown error.");
            errno = err;
            return -1;
        }

        if (err != 0 && err != EISCONN) {
            errno = err;
            if (err == EINPROGRESS || err == EALREADY) {
                LOG_DEBUG("[IO::connect] connect not established yet, keep waiting...");
            }
            return -1;
        }

        // 0, connection has established
        return 0;
    }, timeout_ms);
    
    if (!result) {
        IO::close(sockfd);
        return false;
    }

    if (result.value() != 0) {
        IO::close(sockfd);
        return false;
    }
    
    return true;
}

void IO::close(int fd) {
    if (fd < 0) {
        return;
    }
    
    // LOG_DEBUG("[fiber::IO] Closing fd={}", fd);
    
    // 先取消fd上的所有事件，唤醒等待的fiber
    auto& io_manager = IOManager::getInstance();
    io_manager.cancelAll(fd);
    
    // 再关闭fd
    ::close(fd);
}

} // namespace fiber
