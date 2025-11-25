#include "scheduler.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <thread>
#include "fiber_consumer.h"
#include "io_manager.h"
#include "serika/basic/config_manager.h"
#include "serika/basic/logger.h"
#include "timer.h"

namespace fiber {
Scheduler::Scheduler() : state_(SchedulerState::STOPPED) {
    init(ConfigManager::Instance().get<int>("fiber.num_consumer", 4));
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

    startConsumers(worker_count);
    LOG_DEBUG("Scheduler init ({} workers)", worker_count_);
}

void Scheduler::run() {

    // 多线程模式
    consumers_[0]->consumerLoop();

    LOG_DEBUG("Scheduler stopped");
}

void Scheduler::stop() {
    if (state_ == SchedulerState::STOPPED) {
        return;
    }

    state_ = SchedulerState::STOPPED;

    stopConsumers();
}

bool Scheduler::isRunning() const { return state_ == SchedulerState::RUNNING; }

SchedulerState Scheduler::getState() const { return state_; }

bool Scheduler::hasReadyFibers() const { return !ready_queue_.empty(); }

Scheduler &Scheduler::getInst() {
    static Scheduler scheduler;
    return scheduler;
}

// 多线程调度方法
void Scheduler::scheduleImmediate(const Fiber::ptr& fiber) {
    if (state_ != SchedulerState::RUNNING) {
        LOG_WARN("[Scheduler] pushing fiber when scheduler is not setup, loss fiber!");
        return;
    }

    assert(fiber->getState() != FiberState::DONE &&
           "Scheduling a DONE fiber! This means you got multiple source of a fiber, which is definitely wrong.");

    if (fiber->GetConsumerId().has_value()) {
        FiberConsumer *consumer = consumers_[fiber->GetConsumerId().value()].get();
        assert(consumer->id() == fiber->GetConsumerId().value() && "scheduleImmediate incompatible consumer id.");
        while (!consumer->schedule(fiber)) {
            std::this_thread::yield();
        }
        return;
    }

    // LOG_INFO("Scheduling fiber {}", fiber->getId());
    for (;;) {
        FiberConsumer *consumer = selectConsumer(fiber->GetTraceId());

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

int Scheduler::getWorkerCount() const { return static_cast<int>(consumers_.size()); }

FiberConsumer *Scheduler::getThreadLocalConsumer() {
    auto& scheduler = Scheduler::getInst();
    auto currentFiber = Fiber::current_fiber_;
    assert(currentFiber && currentFiber->GetConsumerId() && "cannot get consumer in non-fiber env");

    return scheduler.consumers_[currentFiber->GetConsumerId().value()].get();
}

IOManager &Scheduler::getThreadLocalIOManager() {
    auto consumer = getThreadLocalConsumer();
    return *(consumer->io_manager_);
}

TimerWheel &Scheduler::getThreadLocalTimerManager() {
    auto consumer = getThreadLocalConsumer();
    return *(consumer->timer_wheel_);
}

FiberConsumer *Scheduler::selectConsumer(uint64_t trace_id) {
    if (consumers_.empty()) {
        return nullptr;
    }

    // 选择队列最短的consumer
    // FiberConsumer *best = nullptr;
    //
    // size_t min_queue_size = 0;
    //
    // for (auto &consumer: consumers_) {
    //     if (consumer->id() == exclude) {
    //         continue;
    //     }
    //
    //     if (!best) {
    //         best = consumer.get();
    //         min_queue_size = consumer->getQueueSize();
    //         continue;
    //     }
    //
    //     size_t queue_size = consumer->getQueueSize();
    //     if (queue_size < min_queue_size) {
    //         min_queue_size = queue_size;
    //         best = consumer.get();
    //     }
    // }
    //
    // return best;

    // Use Hash
    const uint64_t index = trace_id % consumers_.size();
    return consumers_[index].get();
}

void Scheduler::startConsumers(int count) {
    consumers_.clear();

    consumers_.emplace_back(std::make_unique<FiberConsumer>(0, this));
    consumers_[0]->running_ = true;
    for (int i = 1; i < count; ++i) {
        auto consumer = std::make_unique<FiberConsumer>(i, this);
        consumer->start();
        consumers_.push_back(std::move(consumer));
    }
}

void Scheduler::stopConsumers() {
    for (auto &consumer: consumers_) {
        consumer->stop();
    }
    consumers_.clear();
}

} // namespace fiber
