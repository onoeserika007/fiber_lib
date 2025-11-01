//
// Created by inory on 10/30/25.
//

#ifndef FIBER_TIMER_H
#define FIBER_TIMER_H

#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <atomic>
#include "lockfree_queue.h"

namespace fiber {

/**
 * @brief 定时器节点
 * 
 * 单个定时器的数据结构，采用atomic标记实现无锁取消
 */
struct TimerNode {
    using Callback = std::function<void()>;
    using Duration = std::chrono::milliseconds;
    
    Duration timeout;           // 超时时长
    size_t rotations;          // 剩余轮数
    Callback callback;         // 回调函数
    std::atomic<bool> repeat;  // 是否重复
    std::atomic<bool> canceled; // 是否已取消
    
    TimerNode(Duration t, Callback cb, bool rep = false)
        : timeout(t)
        , rotations(0)
        , callback(std::move(cb))
        , repeat(rep)
        , canceled(false) {
    }
};

/**
 * @brief 无锁时间轮定时器
 * 
 * 采用时间轮算法 + lock-free队列实现完全无锁的定时器
 * 设计要点：
 * 1. 使用vector存储每个slot的定时器链表
 * 2. 使用atomic操作保证线程安全
 * 3. 单线程tick，多线程addTimer
 */
class TimerWheel {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::milliseconds;
    using Callback = std::function<void()>;
    using TimerPtr = std::shared_ptr<TimerNode>;

    /**
     * @brief 获取全局时间轮实例
     */
    static TimerWheel& getInstance();

    ~TimerWheel();

    /**
     * @brief 添加定时器（毫秒）
     * @param ms 超时时间（毫秒）
     * @param cb 回调函数
     * @param repeat 是否循环
     * @return 定时器指针
     */
    TimerPtr addTimer(uint64_t ms, Callback cb, bool repeat = false);

    /**
     * @brief 添加定时器（秒，浮点）
     */
    TimerPtr addTimer(double seconds, Callback cb, bool repeat = false) {
        auto ms = static_cast<uint64_t>(seconds * 1000);
        return addTimer(ms, std::move(cb), repeat);
    }

    /**
     * @brief 刷新定时器（重置超时时间）
     * @param timer 要刷新的定时器
     * @return 新的定时器（旧的会被取消）
     * 
     * 注意：在无锁实现中，refresh会取消旧timer并返回一个新的timer，
     *       新timer使用相同的回调和超时时间
     */
    TimerPtr refresh(TimerPtr timer);

    /**
     * @brief 立即触发定时器
     * @param timer 要触发的定时器
     */
    void triggerNow(TimerPtr timer);

    /**
     * @brief 取消定时器
     * @param timer 要取消的定时器
     */
    void cancel(TimerPtr timer);

    /**
     * @brief 时间推进（调度器定期调用）
     * 
     * 处理当前slot的所有到期定时器
     */
    void tick();

    /**
     * @brief 获取tick间隔（毫秒）
     */
    uint64_t getTickInterval() const {
        return tick_interval_.count();
    }

    /**
     * @brief 停止时间轮
     */
    void stop() {
        running_.store(false, std::memory_order_release);
    }

    /**
     * @brief 检查是否在运行
     */
    bool isRunning() const {
        return running_.load(std::memory_order_acquire);
    }

private:
    TimerWheel(size_t slots = 256, Duration tick_interval = Duration(100));

    // 禁止拷贝
    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;

    /**
     * @brief 将定时器添加到指定slot
     */
    void addTimerToSlot(size_t slot, TimerPtr timer);

    /**
     * @brief 处理待添加的定时器队列
     */
    void processPendingTimers();

private:
    // 时间轮配置
    const size_t slots_;                    // slot数量
    const Duration tick_interval_;          // tick间隔
    
    // 时间轮状态（单线程访问，无需原子）
    std::vector<std::vector<TimerPtr>> wheel_;  // 每个slot存储定时器列表
    size_t current_slot_;                       // 当前slot索引
    
    // 待添加队列（多线程安全）
    LockFreeQueue<TimerPtr> pending_timers_;    // 待添加的定时器队列
    
    // 运行状态
    std::atomic<bool> running_;
};

} // namespace fiber

#endif // FIBER_TIMER_H
