#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <sys/epoll.h>
#include <unordered_set>
#include "fiber.h"
#include "wait_queue.h"

#include "sync.h"

namespace fiber {
class FiberConsumer;
}
namespace fiber {

enum class IOEvent { NONE = 0, READ = EPOLLIN, WRITE = EPOLLOUT, READ_WRITE = EPOLLIN | EPOLLOUT };

static constexpr int MAX_FD = 65536;

class IOManager {
public:
    friend FiberConsumer;
    struct FdContext {
        std::unique_ptr<WaitQueue> read_waiters;
        std::unique_ptr<WaitQueue> write_waiters;
        uint32_t events{0};
    };

    using IOCallback = std::function<void()>;
    using FdContextPtr = std::shared_ptr<FdContext>;

    // static IOManager &getInstance();

    ~IOManager();

    void init();
    void shutdown();

    void wakeUpEpoll();
    bool addEvent(int fd, IOEvent event, bool use_et);
    bool delEvent(int fd, IOEvent event, bool use_et);
    bool wakeUp(int fd, IOEvent event);
    auto getFdContext(int fd) const -> FdContextPtr;

    void processEvents(int timeout_ms);

private:
    IOManager();

    bool handleFd(int fd, uint32_t revents);

    int epoll_fd_ {-1};
    int wakeup_fd_ {-1};     // eventfd，用于唤醒 epoll_wait
    std::map<int, FdContextPtr> fd_contexts_;
    std::atomic<bool> running_{false};

    FdContextPtr getOrCreateFdContext(int fd);
};

} // namespace fiber
