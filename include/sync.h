#ifndef FIBER_SYNC_H
#define FIBER_SYNC_H

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <system_error>

#include "serika/basic/logger.h"
#include "fiber.h"
#include "wait_queue.h"

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

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

class alignas(CACHE_LINE_SIZE) SpinLock {
private:
    // 使用 atomic<bool> 而不是 atomic_flag，以便支持更多操作
    std::atomic<bool> lock_ = false;

    // 防止伪共享（False Sharing）
    char padding_[CACHE_LINE_SIZE - sizeof(std::atomic<bool>)]{};

    // 自旋策略参数
    static constexpr uint32_t kMaxSpins = 50;   // 初始快速自旋次数
    static constexpr uint32_t kMaxYields = 10;  // yield 尝试次数

public:
    // 满足 BasicLockable 概念（std::lock_guard 要求）
    void lock() noexcept {
        // 1. 快速路径：尝试一次获取锁
        for (uint32_t i = 0; i < kMaxSpins; ++i) {
            if (try_lock()) {
                return;
            }
            // CPU 友好自旋（x86 上的 PAUSE 指令）
            cpu_relax();
        }

        // 2. 中期：尝试 yield，给其他线程机会
        for (uint32_t i = 0; i < kMaxYields; ++i) {
            if (try_lock()) {
                return;
            }
            std::this_thread::yield();
        }

        // 3. 长期等待：持续自旋直到获取锁
        while (!try_lock()) {
            cpu_relax();
        }
    }

    // 满足 BasicLockable 概念
    void unlock() noexcept {
        // 使用 release 内存序确保临界区操作对其他线程可见
        lock_.store(false, std::memory_order_release);
    }

    // 尝试获取锁（无等待）
    bool try_lock() noexcept {
        // 使用 acquire 内存序确保后续操作不会重排到锁之前
        return !lock_.exchange(true, std::memory_order_acquire);
    }

private:
    // CPU 友好自旋（减少功耗，提高性能）
    static void cpu_relax() noexcept {
        // x86/x64 特定优化：PAUSE 指令
#if defined(__x86_64__) || defined(_M_X64)
        __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
        // ARM 特定优化
        __asm__ volatile("yield" ::: "memory");
#else
        // 通用实现：编译器屏障
        std::atomic_thread_fence(std::memory_order_relaxed);
#endif
    }

public:
    // 禁止拷贝和移动
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    // 构造函数和析构函数
    constexpr SpinLock() noexcept = default;
    ~SpinLock() = default;
};


} // namespace fiber

#endif // FIBER_SYNC_H