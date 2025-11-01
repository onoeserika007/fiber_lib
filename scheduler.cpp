#include "scheduler.h"
#include "fiber_consumer.h"
#include "timer.h"
#include "io_manager.h"
#include "logger.h"
#include <cassert>
#include <iostream>
#include <algorithm>
#include <thread>

namespace fiber {
    Scheduler::Scheduler(): state_(SchedulerState::STOPPED) {
    init(4);
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
        // epoll 正好取代 sleep
        io_manager.processEvents(static_cast<int>(tick_interval_ms));
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
        return;
    }

    // 如果fiber已经完成，直接忽略（这是正常情况，不是错误）
    if (fiber->getState() == FiberState::DONE) {
        return;
    }

    if (FiberConsumer* consumer = selectConsumer()) {
        consumer->schedule(fiber);
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