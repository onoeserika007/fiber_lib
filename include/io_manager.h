#pragma once

#include "fiber.h"
#include "wait_queue.h"
#include <sys/epoll.h>
#include <memory>
#include <functional>
#include <unordered_map>
#include <atomic>

namespace fiber {

enum class IOEvent {
    NONE  = 0,
    READ  = EPOLLIN,
    WRITE = EPOLLOUT
};

class IOManager {
public:
    using IOCallback = std::function<void()>;
    
    static IOManager& getInstance();
    
    ~IOManager();
    
    void init();
    void shutdown();
    
    bool addEvent(int fd, IOEvent event, Fiber::ptr fiber);
    bool delEvent(int fd, IOEvent event);
    bool cancelEvent(int fd, IOEvent event);
    
    void processEvents(int timeout_ms);
    
private:
    IOManager();
    
    struct FdContext {
        std::unique_ptr<WaitQueue> read_waiters;
        std::unique_ptr<WaitQueue> write_waiters;
        uint32_t events{0};
    };
    
    int epoll_fd_{-1};
    std::unordered_map<int, std::unique_ptr<FdContext>> fd_contexts_;
    std::atomic<bool> running_{false};
    
    FdContext* getFdContext(int fd);
    void triggerEvent(int fd, IOEvent event);
};

} // namespace fiber
