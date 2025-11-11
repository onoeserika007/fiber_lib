#include "io_manager.h"
#include "logger.h"
#include <unistd.h>
#include <cstring>

namespace fiber {

IOManager& IOManager::getInstance() {
    static IOManager instance;
    return instance;
}

IOManager::IOManager() = default;

IOManager::~IOManager() {
    shutdown();
}

void IOManager::init() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        LOG_ERROR("epoll_create1 failed: {}", strerror(errno));
        throw std::runtime_error("Failed to create epoll instance");
    }
    
    running_.store(true, std::memory_order_release);
    LOG_DEBUG("IOManager initialized (epoll_fd={})", epoll_fd_);
}

void IOManager::shutdown() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
    
    fd_contexts_.clear();
    LOG_DEBUG("IOManager shutdown");
}

IOManager::FdContext* IOManager::getFdContext(int fd) {
    auto it = fd_contexts_.find(fd);
    if (it != fd_contexts_.end()) {
        return it->second.get();
    }
    
    auto ctx = std::make_unique<FdContext>();
    ctx->read_waiters = std::make_unique<WaitQueue>();
    ctx->write_waiters = std::make_unique<WaitQueue>();
    auto* ptr = ctx.get();
    fd_contexts_[fd] = std::move(ctx);
    return ptr;
}

bool IOManager::addEvent(int fd, IOEvent event, Fiber::ptr fiber) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard latch_ {mu_};
    
    auto* ctx = getFdContext(fd);
    uint32_t old_events = ctx->events;
    uint32_t new_events = old_events | static_cast<uint32_t>(event);
    // LOG_DEBUG("[IOManager] Add Events, fd={}, old_events={}, new_events={}", fd, old_events, new_events);
    
    epoll_event ep_event;
    memset(&ep_event, 0, sizeof(ep_event));
    ep_event.events = new_events | EPOLLET;
    ep_event.data.fd = fd;
    
    int op = old_events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    int ret = epoll_ctl(epoll_fd_, op, fd, &ep_event);
    if (ret < 0) {
        LOG_ERROR("[addEvent] epoll_ctl failed: fd={}, op={}, error={}", fd, op, strerror(errno));
        return false;
    }
    
    ctx->events = new_events;
    
    if (event == IOEvent::READ) {
        ctx->read_waiters->push_back_lockfree(fiber);
    } else if (event == IOEvent::WRITE) {
        ctx->write_waiters->push_back_lockfree(fiber);
    }
    
    return true;
}

bool IOManager::delEvent(int fd, IOEvent event) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard latch_ {mu_};
    
    auto it = fd_contexts_.find(fd);
    if (it == fd_contexts_.end()) {
        return false;
    }
    
    auto* ctx = it->second.get();
    uint32_t old_events = ctx->events;
    uint32_t new_events = old_events & ~static_cast<uint32_t>(event);
    // LOG_DEBUG("[IOManager] Del Event, fd={}, old_events={}, new_events={}", fd, old_events, new_events);
    
    epoll_event ep_event;
    memset(&ep_event, 0, sizeof(ep_event));
    ep_event.events = new_events | EPOLLET;
    ep_event.data.fd = fd;
    
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    int ret = epoll_ctl(epoll_fd_, op, fd, &ep_event);
    if (ret < 0) {
        LOG_ERROR("[delEvent] epoll_ctl failed: fd={}, op={}, error={}", fd, op, strerror(errno));
        return false;
    }
    
    ctx->events = new_events;
    
    if (new_events == 0) {
        fd_contexts_.erase(it);
    }
    
    return true;
}

bool IOManager::cancelEvent(int fd, IOEvent event) {
    // 先唤醒等待的fiber（通知它们事件被取消），再删除epoll注册
    // 顺序很重要：delEvent可能删除FdContext，导致triggerEvent找不到
    triggerEvent(fd, event);
    delEvent(fd, event);
    return true;
}

void IOManager::cancelAll(int fd) {
    // 取消所有可能的事件：READ | WRITE
    cancelEvent(fd, static_cast<IOEvent>(EPOLLIN | EPOLLOUT));
}

void IOManager::triggerEvent(int fd, IOEvent event) {

    std::lock_guard latch_ {mu_};

    auto it = fd_contexts_.find(fd);
    if (it == fd_contexts_.end()) {
        return;
    }
    
    auto* ctx = it->second.get();
    
    uint32_t events = static_cast<uint32_t>(event);
    if (events & EPOLLIN) {
        ctx->read_waiters->notify_all();
    }
    if (events & EPOLLOUT) {
        ctx->write_waiters->notify_all();
    }
}

void IOManager::processEvents(int timeout_ms) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    
    constexpr int MAX_EVENTS = 256;
    epoll_event events[MAX_EVENTS];
    
    int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);
    if (n < 0) {
        if (errno != EINTR) {
            LOG_ERROR("epoll_wait failed: {}", strerror(errno));
        }
        return;
    }
    
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        uint32_t revents = events[i].events;
        
        auto it = fd_contexts_.find(fd);
        if (it == fd_contexts_.end()) {
            continue;
        }
        
        auto* ctx = it->second.get();
        
        if (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
            ctx->read_waiters->notify_all();
        }
        
        if (revents & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
            ctx->write_waiters->notify_all();
        }
    }
}

} // namespace fiber
