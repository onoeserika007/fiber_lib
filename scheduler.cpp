#include "scheduler.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <thread>
#include "serika/basic/logger.h"
#include "fiber_consumer.h"
#include "io_manager.h"
#include "timer.h"

namespace fiber {
    Scheduler::Scheduler(): state_(SchedulerState::STOPPED) {
    init(8);
    // init(std::thread::hardware_concurrency());
    LOG_DEBUG("Scheduler created");
}

Scheduler::~Scheduler() {
    if (state_ != SchedulerState::STOPPED) {
        stop();
    }
    LOG_DEBUG("Scheduler destroyed");
}

void Scheduler::init(int worker_count) {
    assert(state_ == SchedulerState::STOPPED && "Scheduler already running");
    
    state_ = SchedulerState::RUNNING;
    
    // 多线程模式
    startConsumers(worker_count);
    LOG_DEBUG("Scheduler initialized ({} workers)", worker_count);
}

void Scheduler::run() {
    auto& timer_wheel = TimerWheel::getInstance();
    auto& io_manager = IOManager::getInstance();
    uint64_t tick_interval_ms = timer_wheel.getTickInterval();
    
    io_manager.init();
    LOG_DEBUG("Scheduler event loop started (tick interval: {}ms)", tick_interval_ms);
    
    while (state_ == SchedulerState::RUNNING) {
        auto timeout_ms = timer_wheel.getNextTimeOutMs();
        // epoll 正好取代 sleep
        io_manager.processEvents(static_cast<int>(timeout_ms));
        timer_wheel.tick();
    }

    LOG_DEBUG("Scheduler stopping");
    io_manager.shutdown();
    timer_wheel.stop();
    stopConsumers();
    state_ = SchedulerState::STOPPED;
    LOG_DEBUG("Scheduler stopped");
}

void Scheduler::stop() {
    if (state_ == SchedulerState::STOPPED) {
        return;
    }
    
    state_ = SchedulerState::STOPPING;
}

bool Scheduler::isRunning() const {
    return state_ == SchedulerState::RUNNING;
}

SchedulerState Scheduler::getState() const {
    return state_;
}

bool Scheduler::hasReadyFibers() const {
    return !ready_queue_.empty();
}

Scheduler &Scheduler::GetScheduler() {
    static Scheduler scheduler;
    return scheduler;
}

// 多线程调度方法
void Scheduler::scheduleImmediate(Fiber::ptr fiber) {
    if (state_ != SchedulerState::RUNNING) {
        LOG_WARN("[Scheduler] pushing fiber when scheduler is not setup, loss fiber!");
        return;
    }

    assert(fiber->getState() != FiberState::DONE && "Scheduling a DONE fiber! This means you got multiple source of a fiber, which is definitely wrong.");

    // LOG_INFO("Scheduling fiber {}", fiber->getId());
    for (;;) {
        FiberConsumer* consumer = selectConsumer();
        if (!consumer) {
            LOG_ERROR("No consumer to select, fiber lost");
            return;
        }

        if (consumer->schedule(fiber)) {
            // LOG_INFO("Scheduled fiber:{} to consumer:{}", fiber->getId(), consumer->id());
            break;
        }
    }
}

int Scheduler::getWorkerCount() const {
    return static_cast<int>(consumers_.size());
}

FiberConsumer* Scheduler::selectConsumer() {
    if (consumers_.empty()) {
        return nullptr;
    }
    
    // 选择队列最短的consumer
    FiberConsumer* best = consumers_[0].get();
    size_t min_queue_size = best->getQueueSize();
    
    for (auto& consumer : consumers_) {
        size_t queue_size = consumer->getQueueSize();
        if (queue_size < min_queue_size) {
            min_queue_size = queue_size;
            best = consumer.get();
        }
    }
    
    return best;
}

void Scheduler::startConsumers(int count) {
    consumers_.clear();
    
    for (int i = 0; i < count; ++i) {
        auto consumer = std::make_unique<FiberConsumer>(i, this);
        consumer->start();
        consumers_.push_back(std::move(consumer));
    }
}

void Scheduler::stopConsumers() {
    for (auto& consumer : consumers_) {
        consumer->stop();
    }
    consumers_.clear();
}

} // namespace fiber