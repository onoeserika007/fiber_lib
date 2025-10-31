#include "scheduler.h"
#include "fiber_consumer.h"
#include "logger.h"
#include <cassert>
#include <iostream>
#include <algorithm>

namespace fiber {

thread_local Scheduler::ptr Scheduler::thread_scheduler_ = nullptr;

Scheduler::Scheduler(SchedulerMode mode) 
    : mode_(mode), state_(SchedulerState::STOPPED) {
    LOG_DEBUG("Scheduler created ({} mode)", 
              (mode == SchedulerMode::SINGLE_THREAD ? "single-thread" : "multi-thread"));
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
    
    if (mode_ == SchedulerMode::SINGLE_THREAD) {
        // 单线程模式：仅创建main_fiber_供Lua语义使用
        main_fiber_ = Fiber::create(Fiber::FiberFunction{});
        LOG_DEBUG("Scheduler initialized (single-thread mode)");
    } else {
        // 多线程模式：创建fiber消费者
        main_fiber_ = Fiber::create(Fiber::FiberFunction{});
        startConsumers(worker_count);
        LOG_DEBUG("Scheduler initialized (multi-thread mode, {} workers)", worker_count);
    }
}

void Scheduler::stop() {
    if (state_ == SchedulerState::STOPPED) {
        return;
    }
    
    state_ = SchedulerState::STOPPING;
    
    if (mode_ == SchedulerMode::MULTI_THREAD) {
        stopConsumers();
    } else {
        // 单线程模式：运行完所有pending的fiber
        while (hasReadyFibers()) {
            runOnce();
        }
    }
    
    state_ = SchedulerState::STOPPED;
    LOG_DEBUG("Scheduler stopped");
}

bool Scheduler::isRunning() const {
    return state_ == SchedulerState::RUNNING;
}

SchedulerState Scheduler::getState() const {
    return state_;
}

void Scheduler::schedule(Fiber::ptr fiber) {
    if (mode_ == SchedulerMode::SINGLE_THREAD) {
        ready_queue_.push(fiber);
        all_fibers_.push_back(fiber);
    } else {
        // 多线程模式下不应该调用这个方法
        // Go语义应该使用scheduleImmediate
        assert(false && "Use scheduleImmediate for multi-thread mode");
    }
}

void Scheduler::start() {
    assert(mode_ == SchedulerMode::SINGLE_THREAD && "start() only for single-thread mode");
    
    LOG_DEBUG("Scheduler started");
    while (hasReadyFibers()) {
        runOnce();
    }
    LOG_DEBUG("Scheduler finished");
}

void Scheduler::runOnce() {
    if (ready_queue_.empty()) {
        return;
    }
    
    auto fiber = next_ready_fiber();
    if (fiber) {
        fiber->resume();
        
        if (fiber->getState() == FiberState::DONE) {
        } else {
            // Fiber yielded，重新加入队列
            ready_queue_.push(fiber);
        }
        
        cleanup_finished_fibers();
    }
}

bool Scheduler::hasReadyFibers() const {
    return !ready_queue_.empty();
}

// Static methods
Scheduler::ptr Scheduler::GetScheduler() {
    return thread_scheduler_;
}

Scheduler::ptr Scheduler::GetOrCreateScheduler(SchedulerMode mode) {
    if (!thread_scheduler_) {
        thread_scheduler_ = std::make_shared<Scheduler>(mode);
        thread_scheduler_->init();
    }
    return thread_scheduler_;
}

Scheduler::ptr Scheduler::GetOrCreateMultiThreadScheduler() {
    if (!thread_scheduler_ || thread_scheduler_->getMode() != SchedulerMode::MULTI_THREAD) {
        thread_scheduler_ = std::make_shared<Scheduler>(SchedulerMode::MULTI_THREAD);
        thread_scheduler_->init(std::thread::hardware_concurrency());
    }
    return thread_scheduler_;
}

void Scheduler::SetScheduler(Scheduler::ptr scheduler) {
    thread_scheduler_ = scheduler;
}

Fiber::ptr Scheduler::GetMainFiber() {
    auto scheduler = GetOrCreateScheduler();
    return scheduler->main_fiber_;
}

// 多线程调度方法
void Scheduler::scheduleImmediate(Fiber::ptr fiber) {
    assert(mode_ == SchedulerMode::MULTI_THREAD && "scheduleImmediate only for multi-thread mode");
    
    FiberConsumer* consumer = selectConsumer();
    if (consumer) {
        consumer->schedule(fiber);
    }
}

int Scheduler::getWorkerCount() const {
    return static_cast<int>(consumers_.size());
}

// Private methods
void Scheduler::cleanup_finished_fibers() {
    auto it = std::remove_if(all_fibers_.begin(), all_fibers_.end(),
        [](const Fiber::ptr& fiber) {
            return fiber->getState() == FiberState::DONE;
        });
    
    size_t removed = std::distance(it, all_fibers_.end());
    all_fibers_.erase(it, all_fibers_.end());
    
    if (removed > 0) {
        LOG_DEBUG("Cleaned up {} finished fibers", removed);
    }
}

Fiber::ptr Scheduler::next_ready_fiber() {
    if (ready_queue_.empty()) {
        return nullptr;
    }
    
    auto fiber = ready_queue_.front();
    ready_queue_.pop();
    return fiber;
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