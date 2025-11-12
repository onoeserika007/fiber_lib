//
// Created by inory on 10/30/25.
//

#include "timer.h"
#include "logger.h"
#include <algorithm>

namespace fiber {

// ==================== TimerWheel 实现 ====================

TimerWheel& TimerWheel::getInstance() {
    static TimerWheel instance;
    return instance;
}

uint32_t TimerWheel::getNextTimeOutMs() {
    auto now = Clock::now();
    auto elapsed = std::chrono::duration_cast<Duration>(now - last_tick_time_);
    auto remaining = tick_interval_ - elapsed;
    if (remaining.count() <= 0) {
        return 0;
    }
    return static_cast<int>(remaining.count());
}

void TimerWheel::addTimerToSlot(size_t slot, TimerPtr timer) {
}

TimerWheel::TimerWheel(size_t slots, Duration tick_interval)
    : slots_(slots)
    , tick_interval_(tick_interval)
    , wheel_(slots)
    , current_slot_(0)
    , pending_timers_(1024)  // 待添加队列容量
    , running_(true) {
    for (auto& slot : wheel_) {
        slot.reserve(16);
    }
}

TimerWheel::~TimerWheel() {
    stop();
}

TimerWheel::TimerPtr TimerWheel::addTimer(uint64_t ms, Callback cb, bool repeat) {
    if (!running_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    auto timer = std::make_shared<TimerNode>(
        Duration(ms),
        std::move(cb),
        repeat
    );

    // 计算slot和rotations
    uint64_t ticks = ms / tick_interval_.count();
    if (ticks == 0) {
        ticks = 1;  // 至少延迟一个tick
    }

    size_t target_slot = (current_slot_ + ticks) % slots_;
    timer->rotations = ticks / slots_;

    // 添加到待处理队列（无锁）
    while (!pending_timers_.try_push(timer)) {
        // 队列满了，稍微等待
        if (!running_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        // 可以yield或短暂自旋
    }

    return timer;
}

TimerWheel::TimerPtr TimerWheel::refresh(TimerPtr timer) {
    if (!timer || timer->canceled.load(std::memory_order_acquire)) {
        return nullptr;
    }

    // 取消旧timer
    timer->canceled.store(true, std::memory_order_release);
    
    // 创建新timer，使用相同的回调和超时参数
    uint64_t ms = static_cast<uint64_t>(timer->timeout.count());
    return addTimer(
        ms,
        timer->callback,
        timer->repeat.load(std::memory_order_acquire)
    );
}

void TimerWheel::triggerNow(TimerPtr timer) {
    if (!timer) {
        return;
    }

    // 标记为取消
    timer->canceled.store(true, std::memory_order_release);

    // 执行回调
    if (timer->callback) {
        try {
            timer->callback();
        } catch (const std::exception& e) {
            LOG_ERROR("Timer callback exception: {}", e.what());
        } catch (...) {
            LOG_ERROR("Timer callback unknown exception");
        }
    }
}

void TimerWheel::cancel(TimerPtr timer) {
    if (timer) {
        timer->canceled.store(true, std::memory_order_release);
    }
}

void TimerWheel::tick() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    // check real tick
    auto now = Clock::now();
    auto elapsed = std::chrono::duration_cast<Duration>(now - last_tick_time_);
    constexpr auto tolerance = Duration(1);
    if (elapsed + tolerance < tick_interval_) {
        return;
    }

    // 1. 处理待添加的定时器
    processPendingTimers();

    // 2. 处理当前slot的定时器
    auto& bucket = wheel_[current_slot_];
    
    for (auto it = bucket.begin(); it != bucket.end(); ) {
        auto& timer = *it;

        // 如果已取消，移除
        if (timer->canceled.load(std::memory_order_acquire)) {
            it = bucket.erase(it);
            continue;
        }

        // 如果还需要等待轮数，减少rotations
        if (timer->rotations > 0) {
            timer->rotations--;
            ++it;
            continue;
        }

        // 到期了，执行回调
        if (timer->callback) {
            try {
                timer->callback();
            } catch (const std::exception& e) {
                LOG_ERROR("Timer callback exception: {}", e.what());
            } catch (...) {
                LOG_ERROR("Timer callback unknown exception");
            }
        }

        // 如果是循环定时器且未被取消，重新添加
        bool should_repeat = timer->repeat.load(std::memory_order_acquire) &&
                           !timer->canceled.load(std::memory_order_acquire);
        
        if (should_repeat) {
            // 重新计算slot和rotations
            uint64_t ticks = timer->timeout.count() / tick_interval_.count();
            if (ticks == 0) ticks = 1;
            
            size_t target_slot = (current_slot_ + ticks) % slots_;
            timer->rotations = ticks / slots_;
            
            // 如果目标slot不是当前slot，移动到目标slot
            if (target_slot != current_slot_) {
                wheel_[target_slot].push_back(timer);
                it = bucket.erase(it);
            } else {
                // 还在当前slot，保持在这里（但会在下一轮触发）
                timer->rotations++;
                ++it;
            }
        } else {
            // 不重复，移除
            it = bucket.erase(it);
        }
    }

    // 3. 移动到下一个slot
    current_slot_ = (current_slot_ + 1) % slots_;

    // update lack_tick_time
    last_tick_time_ = now;
}

void TimerWheel::processPendingTimers() {
    TimerPtr timer;
    int processed = 0;
    const int max_batch = 100;  // 每次tick最多处理100个待添加定时器

    while (processed < max_batch && pending_timers_.try_pop(timer)) {
        if (!timer || timer->canceled.load(std::memory_order_acquire)) {
            ++processed;
            continue;
        }

        // 计算目标slot
        uint64_t ticks = timer->timeout.count() / tick_interval_.count();
        if (ticks == 0) ticks = 1;
        
        size_t target_slot = (current_slot_ + ticks) % slots_;
        timer->rotations = ticks / slots_;

        // 添加到对应slot
        wheel_[target_slot].push_back(timer);
        ++processed;
    }
}

} // namespace fiber