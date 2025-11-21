#include "io_fiber.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include "fiber.h"
#include "serika/basic/logger.h"

namespace fiber {

static bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

template<typename Func>
std::optional<ssize_t> IO::doIO(int fd, IOEvent event, Func &&op, int64_t timeout_ms) {
    auto current_fiber = Fiber::GetCurrentFiberPtr();
    if (!current_fiber) {
        LOG_ERROR("IO operations must be called from within a fiber");
        return std::nullopt;
    }

    auto &io_manager = IOManager::getInstance();
    auto &timer_wheel = TimerWheel::getInstance();

    auto timeout_state = std::make_shared<std::atomic<bool>>(false);
    auto woken_state = std::make_shared<std::atomic<bool>>(false);

    TimerWheel::TimerPtr timer;
    // timeout_ms == -1 means infinite wait (no timer)
    if (timeout_ms > 0) {
        timer = timer_wheel.addTimer(
                static_cast<uint64_t>(timeout_ms),
                [timeout_state, woken_state, &io_manager, fd, event]() {
                    timeout_state->store(true, std::memory_order_release);
                    if (!woken_state->exchange(true, std::memory_order_acq_rel)) {
                        io_manager.wakeUp(fd, event);
                    }
                },
                false);
    }

    // if (event == IOEvent::READ) {
    //     LOG_INFO("fd:{} is going into the loop", fd);
    // }
    // DEFER({
    //     if (event == IOEvent::READ) {
    //         LOG_INFO("fd:{} is going out of the loop", fd);
    //     }
    // });
    while (true) {
        ssize_t result = op();
        // LOG_INFO("fd:{} is trying to get its result", fd);

        // success or failed not for waiting more data(EAGAIN | EWOULDBLOCK) or wait for connecting, complete this IO
        if (result >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINPROGRESS && errno != EALREADY)) {
            if (timer && !woken_state->exchange(true, std::memory_order_acq_rel)) {
                timer_wheel.cancel(timer);
            }
            // LOG_INFO("fd:{} Get IO Result, return", fd);
            return result;
        }

        // LOG_INFO("fd:{} is checking timeout", fd);

        if (timeout_state->load(std::memory_order_acquire)) {
            errno = ETIMEDOUT;
            // LOG_INFO("fd:{} Get IO Timeout, return", fd);
            return std::nullopt;
        }

        // LOG_INFO("fd:{} is adding event", fd);

        if (!io_manager.addEvent(fd, event, current_fiber)) {
            if (timer && !woken_state->exchange(true, std::memory_order_acq_rel)) {
                timer_wheel.cancel(timer);
            }
            LOG_ERROR("fd:{} Get IO AddEvent Failed, return", fd);
            return std::nullopt;
        }

        // LOG_INFO("fd:{} is going to block", fd);
        // 这里block yield直接丢掉
        Fiber::block_yield();

        // 也有可能epoll触发后，还没有处理，这里就delevent了，可能造成事件的丢失？
        // 不过这个丢了问题应该也不大，只需要保证唤醒的event不丢就行了
        io_manager.delEvent(fd, event);

        if (timeout_state->load(std::memory_order_acquire)) {
            errno = ETIMEDOUT;
            return std::nullopt;
        }
    }
}

std::optional<ssize_t> IO::read(int fd, void *buffer, size_t len, int64_t timeout_ms) {
    setNonBlocking(fd);
    // LOG_DEBUG("[fiber::IO] Calling read by fd={}", fd);
    return doIO(fd, IOEvent::READ, [fd, buffer, len]() { return ::read(fd, buffer, len); }, timeout_ms);
}

std::optional<ssize_t> IO::read_et(int fd, void *buffer, size_t len, int64_t timeout_ms) {
    setNonBlocking(fd);

    auto op = [fd, buffer, len]() -> ssize_t {
        ssize_t total = 0;
        char *ptr = static_cast<char *>(buffer);

        while (true) {
            ssize_t n = ::read(fd, ptr + total, len - total);
            if (n > 0) {
                total += n;
                if (total == static_cast<ssize_t>(len))
                    break; // buffer满了
                continue; // 继续读
            } else if (n == 0) {
                break; // EOF
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                total = total == 0 ? -1 : total;
                break; // 内核缓冲区空
            } else {
                return -1; // 错误
            }
        }

        return total;
    };

    return doIO(fd, IOEvent::READ, std::move(op), timeout_ms);
}

std::optional<ssize_t> IO::write(int fd, const void *buffer, size_t len, int64_t timeout_ms) {
    setNonBlocking(fd);
    // LOG_DEBUG("[fiber::IO] Calling write by fd={}", fd);
    return doIO(fd, IOEvent::WRITE, [fd, buffer, len]() { return ::write(fd, buffer, len); }, timeout_ms);
}

std::optional<int> IO::accept(int sockfd, sockaddr *addr, socklen_t *addrlen, int64_t timeout_ms) {
    setNonBlocking(sockfd);
    // LOG_DEBUG("[fiber::IO] Calling accept by fd={}", sockfd);
    auto result = doIO(
            sockfd, IOEvent::READ, [sockfd, addr, addrlen]() -> ssize_t { return ::accept(sockfd, addr, addrlen); },
            timeout_ms);

    if (result) {
        int client_fd = static_cast<int>(*result);
        setNonBlocking(client_fd); // 新连接的fd也设置为非阻塞
        return client_fd;
    }
    return std::nullopt;
}

std::vector<int> IO::accept_et(int sockfd, sockaddr *addr, socklen_t *addrlen, int64_t timeout_ms) {
    setNonBlocking(sockfd);

    std::vector<int> recvd_fds;
    // LOG_DEBUG("[fiber::IO] Calling accept by fd={}", sockfd);
    doIO(
            sockfd, IOEvent::READ,
            [sockfd, addr, addrlen, &recvd_fds]() -> ssize_t {
                while (true) {
                    ssize_t client_fd = ::accept(sockfd, addr, addrlen);
                    if (client_fd >= 0) {
                        recvd_fds.push_back(client_fd);
                        continue; // 继续读
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // 首次读不到阻塞，之后读完了就应该返回了
                        return recvd_fds.size() == 0 ? -1 : 1;
                    } else if (errno == EMFILE) {
                        LOG_ERROR("[EpollServer] No more fd available: errno={}, error:{}", errno, strerror(errno));
                        continue;
                    } else {
                        LOG_ERROR("[EpollServer] accept failed: errno={}, error:{}", errno, strerror(errno));
                        return -1; // 错误
                    }
                }

                return 1;
            },
            timeout_ms);

    return recvd_fds;
}

bool IO::connect(int sockfd, const sockaddr *addr, socklen_t addrlen, int64_t timeout_ms) {
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
    auto result = doIO(
            sockfd, IOEvent::WRITE,
            [sockfd]() -> ssize_t {
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
            },
            timeout_ms);

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

std::optional<ssize_t> IO::writev(int fd, const iovec *iov, int iovcnt, int64_t timeout_ms) {
    if (fd < 0)
        return std::nullopt;
    setNonBlocking(fd);

    std::vector<iovec> vec(iov, iov + iovcnt);

    size_t total_len = 0;
    for (int i = 0; i < iovcnt; ++i) {
        total_len += vec[i].iov_len;
    }

    // 用于记录已写字节
    size_t written = 0;

    auto op = [fd, vec = std::move(vec), &written, total_len]() mutable -> ssize_t {
        // construct tmp
        std::vector<iovec> tmp;
        tmp.reserve(vec.size());
        for (auto &v: vec) {
            if (v.iov_len > 0)
                tmp.push_back(v);
        }

        ssize_t n = ::writev(fd, tmp.data(), tmp.size());

        // update offset
        if (n > 0) {
            written += static_cast<size_t>(n);

            auto remain = static_cast<size_t>(n);

            for (auto &v: vec) {
                if (remain == 0)
                    break;

                if (remain >= v.iov_len) {
                    remain -= v.iov_len;
                    v.iov_len = 0;
                } else {
                    v.iov_base = static_cast<char *>(v.iov_base) + remain;
                    v.iov_len -= remain;
                    remain = 0;
                }
            }
        }

        if (n < 0) {
            return n;
        }

        if (total_len == written) {
            return n;
        }

        // keep write
        errno = EAGAIN;
        n = -1;
        return n; // 让 doIO 判断是否 yield
    };

    // 交给 doIO：它会在 EAGAIN 时挂起，恢复后再次执行 op()，直到完全写完或失败
    auto ret = doIO(fd, IOEvent::WRITE, std::move(op), timeout_ms);

    if (!ret)
        return std::nullopt;

    // ret 是最终那次 writev 的返回值，而不是总写入量，因此返回 *written
    return static_cast<ssize_t>(written);
}

std::optional<ssize_t> IO::sendfile(int out_fd, int in_fd, off_t *offset, size_t count, int64_t timeout_ms) {
    setNonBlocking(out_fd);

    return doIO(
            out_fd, IOEvent::WRITE,
            [out_fd, in_fd, offset, count]() -> ssize_t { return ::sendfile(out_fd, in_fd, offset, count); },
            timeout_ms);
}

std::optional<ssize_t> IO::recv(int fd, void *buffer, size_t len, int flags, int64_t timeout_ms) {
    setNonBlocking(fd);

    auto op = [fd, buffer, len, flags]() -> ssize_t { return ::recv(fd, buffer, len, flags); };

    return doIO(fd, IOEvent::READ, op, timeout_ms);
}

std::optional<ssize_t> IO::recv_et(int fd, void *buffer, size_t len, int flags, int64_t timeout_ms) {
    setNonBlocking(fd);

    auto op = [fd, buffer, len, flags]() -> ssize_t {
        ssize_t total = 0;
        char *ptr = static_cast<char *>(buffer);

        while (true) {
            ssize_t n = ::recv(fd, ptr + total, len - total, flags);
            if (n > 0) {
                total += n;
                if (total == static_cast<ssize_t>(len))
                    break; // buffer 满
                continue; // 继续读
            } else if (n == 0) {
                break; // 对端关闭连接
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 需要 n = -1 和 EAGAIN EWOULDBLOCK来严格区分 FIN和内核缓存区读空
                total = total == 0 ? -1 : total;
                break; // 内核缓冲区已空
            } else {
                return -1; // 其他错误
            }
        }
        return total;
    };

    return doIO(fd, IOEvent::READ, op, timeout_ms);
}

int IO::close(int fd) {

    // LOG_DEBUG("[fiber::IO] Closing fd={}", fd);

    // 先取消fd上的所有事件，唤醒等待的fiber
    auto &io_manager = IOManager::getInstance();
    // 这里还真应该都唤醒的
    io_manager.wakeUpAll(fd);

    // 再关闭fd
    return ::close(fd);
}

int IO::shutdown(int fd, int how) {

    auto &io_manager = IOManager::getInstance();
    if (how == SHUT_RD) {
        io_manager.wakeUp(fd, IOEvent::READ);
    } else if (how == SHUT_WR) {
        io_manager.wakeUp(fd, IOEvent::WRITE);
    } else if (how == SHUT_RDWR) {
        io_manager.wakeUp(fd, IOEvent::READ);
        io_manager.wakeUp(fd, IOEvent::WRITE);
    }

    return ::shutdown(fd, how); // 非阻塞调用
}

} // namespace fiber
