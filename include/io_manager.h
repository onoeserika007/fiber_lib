#pragma once

#include "fiber.h"
#include "wait_queue.h"
#include <sys/epoll.h>
#include <memory>
#include <functional>
#include <atomic>
#include <unordered_set>

#include "sync.h"

namespace fiber {

enum class IOEvent {
    NONE  = 0,
    READ  = EPOLLIN,
    WRITE = EPOLLOUT
};

static constexpr int MAX_FD = 65536;

class IOManager {
public:
    struct FdContext {
        std::unique_ptr<WaitQueue> read_waiters;
        std::unique_ptr<WaitQueue> write_waiters;
        uint32_t events{0};

        fiber::SpinLock fd_mu;
    };

    using IOCallback = std::function<void()>;
    using FdContextPtr = std::shared_ptr<FdContext>;
    
    static IOManager& getInstance();
    
    ~IOManager();
    
    void init();
    void shutdown();
    
    bool addEvent(int fd, IOEvent event, Fiber::ptr fiber);
    bool delEvent(int fd, IOEvent event);
    void delAll(int fd);  // 取消fd上的所有事件
    bool wakeUp(int fd, IOEvent event);
    void wakeUpAll(int fd);
    auto getFdContext(int fd) const -> FdContextPtr;
    
    void processEvents(int timeout_ms);
    
private:
    IOManager();

    bool handleFd(int fd, uint32_t revents);
    
    int epoll_fd_{-1};
    std::vector<std::atomic<FdContextPtr>> fd_contexts_{MAX_FD};
    std::atomic<bool> running_{false};
    fiber::FiberMutex mu_;
    
    FdContextPtr getOrCreateFdContext(int fd);
    void triggerEvent(int fd, IOEvent event);

    std::string events_to_string(epoll_event events[], int n);
    int getFdContextNum() const;
    int total_events_ = 0;
    int add_events_call_counts_ = 0;
    std::unordered_set<int> history_fd_ {};
};

} // namespace fiber
