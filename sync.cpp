#include "include/sync.h"
#include "include/scheduler.h"

namespace fiber {

FiberMutex::FiberMutex() 
    : locked_(false), owner_(nullptr), waiters_(std::make_unique<WaitQueue>()) {
}

FiberMutex::~FiberMutex() {
    // 确保析构时锁已释放
    if (locked_.load(std::memory_order_acquire)) {
        LOG_WARN("FiberMutex destroyed while still locked!");
    }
}

void FiberMutex::lock() {
    // 快速路径：尝试直接获取锁
    while (!try_acquire_lock()) {
        // 慢速路径：进入等待队列并挂起当前fiber
        LOG_DEBUG("FiberMutex::lock() - fiber waiting for lock");
        waiters_->wait();
        // 被唤醒后继续循环尝试获取锁
    }
}

bool FiberMutex::try_lock() {
    return try_acquire_lock();
}

void FiberMutex::unlock() {
    if (!locked_.load(std::memory_order_acquire)) {
        throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                              "Attempting to unlock an unlocked mutex");
    }
    
    auto current = Fiber::GetCurrentFiberPtr();
    Fiber* expected_owner = current.get();
    if (!current || owner_.load(std::memory_order_acquire) != expected_owner) {
        throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                              "Attempting to unlock a mutex not owned by current fiber");
    }
    
    release_lock_internal();
    
    // 通知一个等待的协程（WaitQueue内部是lock-free的）
    waiters_->notify_one();
}

bool FiberMutex::is_locked_by_current() const {
    if (!locked_.load(std::memory_order_acquire)) {
        return false;
    }
    
    auto current = Fiber::GetCurrentFiberPtr();
    return current && owner_.load(std::memory_order_acquire) == current.get();
}

bool FiberMutex::try_acquire_lock() {
    auto current = Fiber::GetCurrentFiberPtr();
    if (!current) {
        LOG_ERROR("FiberMutex::try_acquire_lock() - not called from within a fiber");
        return false;
    }
    
    // 使用CAS原子操作尝试获取锁
    bool expected = false;
    if (locked_.compare_exchange_strong(expected, true, 
                                        std::memory_order_acquire, 
                                        std::memory_order_relaxed)) {
        // 成功获取锁，设置owner
        owner_.store(current.get(), std::memory_order_release);
        LOG_DEBUG("FiberMutex::try_acquire_lock() - lock acquired by fiber");
        return true;
    }
    
    return false;
}

void FiberMutex::release_lock_internal() {
    owner_.store(nullptr, std::memory_order_release);
    locked_.store(false, std::memory_order_release);
    LOG_DEBUG("FiberMutex::release_lock_internal() - lock released");
}

// FiberCondition 实现
FiberCondition::FiberCondition() 
    : waiters_(std::make_unique<WaitQueue>()) {
}

FiberCondition::~FiberCondition() = default;

void FiberCondition::wait(std::unique_lock<FiberMutex>& lock) {
    if (!lock.owns_lock()) {
        throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                              "Condition variable wait called without owning the lock");
    }
    
    // 释放锁并等待通知（原子操作）
    lock.unlock();
    
    LOG_DEBUG("FiberCondition::wait() - fiber waiting for condition");
    waiters_->wait();
    
    // 重新获取锁
    lock.lock();
}

void FiberCondition::notify_one() {
    // WaitQueue内部已经是lock-free的，直接调用
    waiters_->notify_one();
}

void FiberCondition::notify_all() {
    // WaitQueue内部已经是lock-free的，直接调用
    waiters_->notify_all();
}

// WaitGroup 实现
WaitGroup::WaitGroup() 
    : counter_(0), waiters_(std::make_unique<WaitQueue>()) {
}

WaitGroup::~WaitGroup() = default;

void WaitGroup::add(int delta) {
    // 使用fetch_add原子操作更新计数器
    int old_count = counter_.fetch_add(delta, std::memory_order_acq_rel);
    int new_count = old_count + delta;
    
    if (new_count < 0) {
        // 计数器不能为负数，需要回滚
        counter_.fetch_sub(delta, std::memory_order_release);
        throw std::invalid_argument("WaitGroup counter cannot be negative");
    }
    
    LOG_DEBUG("WaitGroup::add({}) - counter: {} -> {}", delta, old_count, new_count);
    
    // 如果计数器归零，通知所有等待者
    if (new_count == 0) {
        notify_waiters_if_done();
    }
}

void WaitGroup::done() {
    add(-1);
}

void WaitGroup::wait() {
    // 快速路径：如果已经完成，直接返回
    if (counter_.load(std::memory_order_acquire) == 0) {
        return;
    }
    
    LOG_DEBUG("WaitGroup::wait() - waiting for counter to reach zero");
    
    // 进入等待队列，挂起当前fiber
    waiters_->wait();
    
    // 注意：由于可能有spurious wakeup（虚假唤醒），被唤醒后需要再次检查
    // 但是由于我们的实现保证只在counter==0时才notify_all，所以这里不需要循环
}

int WaitGroup::count() const {
    return counter_.load(std::memory_order_acquire);
}

void WaitGroup::notify_waiters_if_done() {
    if (counter_.load(std::memory_order_acquire) == 0) {
        LOG_DEBUG("WaitGroup::notify_waiters_if_done() - notifying all waiters");
        waiters_->notify_all();
    }
}

} // namespace fiber