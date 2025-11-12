#ifndef FIBER_SYNC_H
#define FIBER_SYNC_H

#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <system_error>
#include "fiber.h"
#include "wait_queue.h"
#include "../../../third_party/basic_libs/include/logger.h"

namespace fiber {

/**
 * @brief 协程级互斥量
 * 
 * 与std::mutex不同，FiberMutex不会阻塞操作系统线程，
 * 而是挂起当前协程，让其他协程可以在同一线程中继续执行
 */
class FiberMutex {
public:
    FiberMutex();
    ~FiberMutex();
    
    // 禁用拷贝和移动
    FiberMutex(const FiberMutex&) = delete;
    FiberMutex& operator=(const FiberMutex&) = delete;
    FiberMutex(FiberMutex&&) = delete;
    FiberMutex& operator=(FiberMutex&&) = delete;
    
    /**
     * @brief 获取锁（阻塞操作）
     * 如果锁已被占用，当前协程会被挂起直到锁可用
     */
    void lock();
    
    /**
     * @brief 尝试获取锁（非阻塞操作）
     * @return 是否成功获取锁
     */
    bool try_lock();
    
    /**
     * @brief 释放锁
     */
    void unlock();
    
    /**
     * @brief 带超时的锁获取（为迭代5预留）
     */
    template<typename Rep, typename Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout) {
        // 为迭代5预留的超时实现
        // 目前简单返回try_lock的结果
        return try_lock();
    }

private:
    std::atomic<bool> locked_;                // 锁状态
    std::unique_ptr<WaitQueue> waiters_;      // 等待获取锁的协程队列
    
    // 内部辅助方法
    bool try_acquire_lock();
};

/**
 * @brief 协程级条件变量
 * 
 * 与FiberMutex配合使用，提供协程间的条件等待和通知机制
 */
class FiberCondition {
public:
    FiberCondition();
    ~FiberCondition();
    
    // 禁用拷贝和移动
    FiberCondition(const FiberCondition&) = delete;
    FiberCondition& operator=(const FiberCondition&) = delete;
    
    /**
     * @brief 等待条件满足
     * @param lock 与条件变量关联的互斥锁
     */
    void wait(std::unique_lock<FiberMutex>& lock);
    
    /**
     * @brief 带谓词的条件等待
     * @param lock 互斥锁
     * @param predicate 条件谓词，返回true时停止等待
     */
    template<typename Predicate>
    void wait(std::unique_lock<FiberMutex>& lock, Predicate predicate) {
        while (!predicate()) {
            wait(lock);
        }
    }
    
    /**
     * @brief 带超时的条件等待
     * @param lock 持有的锁
     * @param timeout 超时时长
     * @return true表示被notify唤醒，false表示超时
     */
    template<typename Rep, typename Period>
    bool wait_for(std::unique_lock<FiberMutex>& lock, 
                  const std::chrono::duration<Rep, Period>& timeout);
    
    /**
     * @brief 带超时和谓词的条件等待
     * @param lock 持有的锁
     * @param timeout 超时时长
     * @param predicate 谓词函数
     * @return true表示谓词满足，false表示超时
     */
    template<typename Rep, typename Period, typename Predicate>
    bool wait_for(std::unique_lock<FiberMutex>& lock,
                  const std::chrono::duration<Rep, Period>& timeout,
                  Predicate predicate) {
        while (!predicate()) {
            if (!wait_for(lock, timeout)) {
                return predicate();  // 超时后最后检查一次
            }
        }
        return true;
    }
    
    /**
     * @brief 通知一个等待的协程
     */
    void notify_one();
    
    /**
     * @brief 通知所有等待的协程
     */
    void notify_all();

private:
    std::unique_ptr<WaitQueue> waiters_;      // 等待条件的协程队列
};

/**
 * @brief Go风格的WaitGroup
 * 
 * 用于等待一组协程完成
 */
class WaitGroup {
public:
    WaitGroup();
    ~WaitGroup();
    
    // 禁用拷贝和移动
    WaitGroup(const WaitGroup&) = delete;
    WaitGroup& operator=(const WaitGroup&) = delete;
    
    /**
     * @brief 增加等待计数
     * @param delta 增加的数量（可以为负数，表示减少）
     */
    void add(int delta = 1);
    
    /**
     * @brief 完成一个任务（等价于add(-1)）
     */
    void done();
    
    /**
     * @brief 等待所有任务完成
     * 当计数器归零时返回
     */
    void wait();
    
    /**
     * @brief 获取当前计数
     */
    int count() const;

private:
    std::atomic<int> counter_;                // 等待计数器
    std::unique_ptr<WaitQueue> waiters_;      // 等待完成的协程队列
    
    void notify_waiters_if_done();
};

/**
 * @brief RAII风格的协程锁守护
 */
template<typename Mutex>
class lock_guard {
public:
    explicit lock_guard(Mutex& mutex) : mutex_(mutex) {
        mutex_.lock();
    }
    
    ~lock_guard() {
        mutex_.unlock();
    }
    
    // 禁用拷贝和移动
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

private:
    Mutex& mutex_;
};

/**
 * @brief 协程版本的unique_lock
 */
template<typename Mutex>
class unique_lock {
public:
    unique_lock() : mutex_(nullptr), owns_lock_(false) {}
    
    explicit unique_lock(Mutex& mutex) : mutex_(&mutex), owns_lock_(false) {
        lock();
    }
    
    ~unique_lock() {
        if (owns_lock_) {
            unlock();
        }
    }
    
    // 移动构造和赋值
    unique_lock(unique_lock&& other) noexcept 
        : mutex_(other.mutex_), owns_lock_(other.owns_lock_) {
        other.mutex_ = nullptr;
        other.owns_lock_ = false;
    }
    
    unique_lock& operator=(unique_lock&& other) noexcept {
        if (this != &other) {
            if (owns_lock_) {
                unlock();
            }
            mutex_ = other.mutex_;
            owns_lock_ = other.owns_lock_;
            other.mutex_ = nullptr;
            other.owns_lock_ = false;
        }
        return *this;
    }
    
    // 禁用拷贝
    unique_lock(const unique_lock&) = delete;
    unique_lock& operator=(const unique_lock&) = delete;
    
    void lock() {
        if (!mutex_) throw std::system_error(std::make_error_code(std::errc::operation_not_permitted));
        if (owns_lock_) throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur));
        
        mutex_->lock();
        owns_lock_ = true;
    }
    
    bool try_lock() {
        if (!mutex_) throw std::system_error(std::make_error_code(std::errc::operation_not_permitted));
        if (owns_lock_) throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur));
        
        owns_lock_ = mutex_->try_lock();
        return owns_lock_;
    }
    
    void unlock() {
        if (!owns_lock_) throw std::system_error(std::make_error_code(std::errc::operation_not_permitted));
        
        mutex_->unlock();
        owns_lock_ = false;
    }
    
    bool owns_lock() const noexcept { return owns_lock_; }
    
    Mutex* mutex() const noexcept { return mutex_; }

private:
    Mutex* mutex_;
    bool owns_lock_;
};

// 类型别名，方便使用
using fiber_lock_guard = lock_guard<FiberMutex>;
using fiber_unique_lock = unique_lock<FiberMutex>;

} // namespace fiber

#endif // FIBER_SYNC_H