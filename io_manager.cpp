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

// atomic
IOManager::FdContextPtr IOManager::getOrCreateFdContext(int fd) {
    // 1st check: fast path
    auto ctx = fd_contexts_[fd].load(std::memory_order_acquire);
    if (ctx) {
        return ctx;
    }

    // Prepare a new context (not yet published)
    auto new_ctx = std::make_shared<FdContext>();
    new_ctx->read_waiters = std::make_unique<WaitQueue>();
    new_ctx->write_waiters = std::make_unique<WaitQueue>();

    // 2nd check + CAS
    std::shared_ptr<FdContext> expected = nullptr;  // must be null
    if (fd_contexts_[fd].compare_exchange_strong(
            expected,
            new_ctx,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        // CAS 成功，new_ctx 被放入表中
        return new_ctx;
    }

    // CAS 失败，说明别人已经成功创建了，expected 会被写为当前实际值
    return expected;
}

auto IOManager::getFdContext(int fd) const -> FdContextPtr {
    return fd_contexts_[fd].load(std::memory_order_relaxed);
}

bool IOManager::addEvent(int fd, IOEvent event, Fiber::ptr fiber) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    // LOG_DEBUG("[IOManager] Add Events, fd:{}, getting latch", fd);
    auto ctx = getOrCreateFdContext(fd);

    std::unique_lock fd_lock {ctx->fd_mu};

    uint32_t old_events = ctx->events;
    uint32_t new_events = old_events | static_cast<uint32_t>(event);
    // LOG_INFO("[IOManager] Add Events, fd:{}, old_events={}, new_events={}", fd, old_events, new_events);
    
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
    
    return true;
}

bool IOManager::delEvent(int fd, IOEvent event) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    // LOG_INFO("[IOManager] Del Events, fd:{}, getting latch", fd);

    
    auto ctx = fd_contexts_[fd].load(std::memory_order_acquire);

    if (!ctx) {
        return false;
    }

    std::unique_lock fd_lock {ctx->fd_mu};
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
        fd_contexts_[fd].store(nullptr, std::memory_order_release);
    }
    
    return true;
}

// TODO cancel
// event不应该通知吧？想想通知的场景，应该是超时了才会触发，不超时提前取消说明已经不阻塞了
bool IOManager::wakeUp(int fd, IOEvent event) {
    // 先唤醒等待的fiber（通知它们事件被取消），再删除epoll注册
    // 顺序很重要：delEvent可能删除FdContext，导致triggerEvent找不到
    triggerEvent(fd, event);
    delEvent(fd, event);
    return true;
}

void IOManager::delAll(int fd) {
    // 取消所有可能的事件：READ | WRITE
    delEvent(fd, static_cast<IOEvent>(EPOLLIN | EPOLLOUT));
}

void IOManager::triggerEvent(int fd, IOEvent event) {

    auto ctx = fd_contexts_[fd].load(std::memory_order_acquire);
    if (!ctx) {
        return;
    }
    
    uint32_t events = static_cast<uint32_t>(event);
    if (events & EPOLLIN) {
        ctx->read_waiters->notify_all();
    }
    if (events & EPOLLOUT) {
        ctx->write_waiters->notify_all();
    }
}

std::string IOManager::events_to_string(epoll_event events[], int n) {
    std::ostringstream os;
    os << "[";
    if (n > 0) {
        os << events[0].data.fd << "(" << events[0].events << ")";
    }
    for (int i = 1; i < n; i++) {
        os << ", " << events[i].data.fd << "(" << events[i].events << ")";
    }
    os << "]";
    return os.str();
}

int IOManager::getFdContextNum() const {
}

void IOManager::processEvents(int timeout_ms) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    
    constexpr int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];
    
    int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);
    if (n < 0) {
        if (errno != EINTR) {
            LOG_ERROR("epoll_wait failed: {}", strerror(errno));
        }
        return;
    }

    total_events_ += n;
    // LOG_INFO("Epoll received {} events: {}, total {}", n, events_to_string(events, n), total_events_);
    // LOG_INFO("Fd Contexts size:{}", fd_contexts_.size());
    // LOG_INFO("Add Events call counts:{}", add_events_call_counts_);
    
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        uint32_t revents = events[i].events;

        // TODO unordered_map并发不安全，
        /*任何两个线程，只要有一个线程对容器做写操作，另一个线程同时访问该容器就是未定义行为。*/
        // 1.改成vector一张大表
        // 2.改成向工作线程发送信号
        auto ctx = fd_contexts_[fd].load(std::memory_order_acquire);
        if (!ctx) {
            LOG_WARN("fd:{} cannot find fd context, event may have lost", fd);
            continue;
        }
        
        if (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
            // LOG_INFO("Waking up a reader:{}", fd);
            ctx->read_waiters->notify_all();
        }
        
        if (revents & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
            ctx->write_waiters->notify_all();
        }
    }
}

} // namespace fiber
