#include "io_manager.h"
#include <cstring>
#include <unistd.h>
#include <sys/eventfd.h>

#include "serika/basic/logger.h"

namespace fiber {

// IOManager &IOManager::getInstance() {
//     static IOManager instance;
//     return instance;
// }

IOManager::IOManager() = default;

IOManager::~IOManager() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (wakeup_fd_ >= 0) {
        close(wakeup_fd_);
    }

    fd_contexts_.clear();
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

    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0) {
        LOG_ERROR("Failed to create eventfd: {}", strerror(errno));
        close(epoll_fd_);
        throw std::runtime_error("Failed to create eventfd");
    }

    // 将 eventfd 注册到 epoll
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = wakeup_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev) < 0) {
        LOG_ERROR("Failed to add eventfd to epoll: {}", strerror(errno));
        close(epoll_fd_);
        close(wakeup_fd_);
        throw std::runtime_error("Failed to add eventfd");
    }

    running_.store(true, std::memory_order_release);
    LOG_DEBUG("IOManager initialized (epoll_fd={})", epoll_fd_);
}

void IOManager::shutdown() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // 唤醒 epoll_wait
    uint64_t val = 1;
    write(wakeup_fd_, &val, sizeof(val));
    LOG_DEBUG("IOManager shutdown");
}

void IOManager::wakeUpEpoll() {
    // 唤醒 epoll_wait
    uint64_t val = 1;
    ssize_t n = write(wakeup_fd_, &val, sizeof(val));
    if (n != sizeof(val)) {
        LOG_ERROR("Failed to wake up epoll", strerror(errno));
    }
}

// atomic
IOManager::FdContextPtr IOManager::getOrCreateFdContext(int fd) {
    // 现在不需要任何并发保护，IOManager单线程内运行
    if (fd_contexts_.contains(fd)) {
        return fd_contexts_.at(fd);
    }

    // Prepare a new context (not yet published)
    fd_contexts_[fd] = std::make_shared<FdContext>();
    fd_contexts_[fd]->read_waiters = std::make_unique<WaitQueue>();
    fd_contexts_[fd]->write_waiters = std::make_unique<WaitQueue>();
    return fd_contexts_[fd];
}

auto IOManager::getFdContext(int fd) const -> FdContextPtr {
    if (fd_contexts_.contains(fd)) {
        return fd_contexts_.at(fd);
    }
    return {};
}

bool IOManager::addEvent(int fd, IOEvent event, bool use_et) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    // LOG_DEBUG("[IOManager] Add Events, fd:{}, getting latch", fd);
    auto ctx = getOrCreateFdContext(fd);


    uint32_t old_events = ctx->events;
    uint32_t new_events = old_events | static_cast<uint32_t>(event);
    LOG_DEBUG("[IOManager] Add Events, fd:{}, old_events={}, new_events={}", fd, old_events, new_events);

    epoll_event ep_event;
    memset(&ep_event, 0, sizeof(ep_event));
    ep_event.events = new_events | EPOLLONESHOT;
    if (use_et) {
        ep_event.events |= EPOLLET;
    }
    ep_event.data.fd = fd;

    // 有可能一种竞态条件导致事件丢失，在注册epoll事件后，到开始阻塞前，触发了epoll
    // 所以这里要拿住fd_mutex

    int op = old_events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    assert(op == EPOLL_CTL_ADD && "Invalid Op");
    int ret = epoll_ctl(epoll_fd_, op, fd, &ep_event);
    if (ret < 0) {
        LOG_ERROR("[addEvent] epoll_ctl failed: fd={}, op={}, error={}", fd, op, strerror(errno));
        return false;
    }

    ctx->events = new_events;

    auto current_fiber = Fiber::GetCurrentFiberPtr();
    assert(current_fiber && "IO operations must be called from within a fiber");

    // 原来会触发两次的bug是因为在addEvent那里也写了如下逻辑
    if (event == IOEvent::READ) {
        ctx->read_waiters->push_back_lockfree(current_fiber);
    } else if (event == IOEvent::WRITE) {
        ctx->write_waiters->push_back_lockfree(current_fiber);
    }

    return true;
}

bool IOManager::delEvent(int fd, IOEvent event, bool use_et) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    // LOG_INFO("[IOManager] Del Events, fd:{}, getting latch", fd);

    if (!fd_contexts_.contains(fd)) {
        return false;
    }

    auto ctx = fd_contexts_.at(fd);

    uint32_t old_events = ctx->events;
    uint32_t new_events = old_events & ~static_cast<uint32_t>(event);
    LOG_DEBUG("[IOManager] Del Event, fd={}, old_events={}, new_events={}", fd, old_events, new_events);

    epoll_event ep_event;
    memset(&ep_event, 0, sizeof(ep_event));
    ep_event.events = new_events;
    if (use_et) {
        ep_event.events |= EPOLLET;
    }
    ep_event.data.fd = fd;

    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    int ret = epoll_ctl(epoll_fd_, op, fd, &ep_event);
    if (ret < 0) {
        LOG_ERROR("[delEvent] epoll_ctl failed: fd={}, op={}, error={}", fd, op, strerror(errno));
        return false;
    }

    ctx->events = new_events;

    if (new_events == 0) {
        fd_contexts_.erase(fd);
    } else {
        LOG_DEBUG("Delevent remaining");
    }

    return true;
}

// TODO cancel
// event不应该通知吧？想想通知的场景，应该是超时了才会触发，不超时提前取消说明已经不阻塞了
bool IOManager::wakeUp(int fd, IOEvent event) {

    if (!fd_contexts_.contains(fd)) {
        return false;
    }
    auto ctx = fd_contexts_.at(fd);

    uint32_t events = static_cast<uint32_t>(event);
    if (events & EPOLLIN) {
        ctx->read_waiters->notify_all();
    }
    if (events & EPOLLOUT) {
        ctx->write_waiters->notify_all();
    }

    return true;
}

bool IOManager::handleFd(int fd, uint32_t revents) {
    // TODO unordered_map并发不安全，
    /*任何两个线程，只要有一个线程对容器做写操作，另一个线程同时访问该容器就是未定义行为。*/
    // 1.改成vector一张大表
    // 2.改成向工作线程发送信号
    if (fd == wakeup_fd_) {
        return true;
    }

    if (!fd_contexts_.contains(fd)) {
        LOG_WARN("fd:{} cannot find fd context, event may have lost", fd);
        return false;
    }

    auto ctx = fd_contexts_.at(fd);

    if (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
        // LOG_INFO("Waking up a reader:{}", fd);
        ctx->read_waiters->notify_all();
    }

    if (revents & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
        ctx->write_waiters->notify_all();
    }

    return true;
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

    std::queue<std::pair<int, uint32_t>> remains;
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        uint32_t revents = events[i].events;

        handleFd(fd, revents);

    }
}

} // namespace fiber
