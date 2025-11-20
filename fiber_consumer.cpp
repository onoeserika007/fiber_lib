#include <chrono>
#include <iostream>
#include <thread>

#include "fiber_consumer.h"
#include "serika/basic/logger.h"
#include "scheduler.h"

namespace fiber {

FiberConsumer::FiberConsumer(int id, Scheduler* scheduler) 
    : id_(id)
    , scheduler_(scheduler)
    // , queue_(std::make_unique<moodycamel::ConcurrentQueue<std::shared_ptr<Fiber>>>()) {
    , queue_(std::make_unique<LockFreeLinkedList<std::shared_ptr<Fiber>>>()) {
}

FiberConsumer::~FiberConsumer() {
    stop();
}

void FiberConsumer::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return; // 已经在运行
    }
    
    thread_ = std::thread(&FiberConsumer::consumerLoop, this);
}

void FiberConsumer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return; // 已经停止
    }

    // 这里一直报错是因为我在用FiberConsumer线程自己join自己，当然会出错了
    if (thread_.joinable()) {
        thread_.join();
    }

    // Fiber::ptr task;
    // while (queue_->try_dequeue(task)) {
    //     task->resume();
    // }
    while (auto task = queue_->pop_front_lockfree().value_or(nullptr)) {
        task->resume();
    }
}

bool FiberConsumer::schedule(Fiber::ptr fiber) {
    if (!running_.load(std::memory_order_acquire)) {
        LOG_WARN("[FiberConsumer] pushing fiber when FiberConsumer is not setup, loss fiber!");
        return true;
    }
    
    // 这里自旋的话会造成饥饿，因为分配任务的协程有可能是被选中的协程
    // 并发特别大的话就容易这样，因此需要把自旋挪到外面去
    // return queue_->try_enqueue(fiber);
    queue_->push_back_lockfree(fiber);
    return true;
}

size_t FiberConsumer::getQueueSize() const { return queue_->size(); }

int FiberConsumer::id() const { return id_; }

void FiberConsumer::consumerLoop() {
    LOG_DEBUG("FiberConsumer {} started", id_);
    
    while (running_.load(std::memory_order_acquire)) {
        processTask();
    }

    Fiber::ResetMainFiber();
    LOG_DEBUG("FiberConsumer {} stopped", id_);
}

void FiberConsumer::processTask() {
    // Lock-free地从队列获取任务
    Fiber::ptr task {};
    // if (!queue_->try_dequeue(task)) {
    //     // std::this_thread::sleep_for(std::chrono::duration<int64_t, std::milli>(20));
    //     std::this_thread::yield();
    //     return;
    // }

    task = queue_->pop_front_lockfree().value_or(nullptr);
    
    if (!task) {
        std::this_thread::yield();
        return;
    }

    // LOG_INFO("[FiberConsumer::processTask] resuming a fiber");

    // auto parent_fiber = task->getParentFiber();
    // assert(parent_fiber.get() == nullptr && "A scheduling fiber can't have parent!");
    
    // 执行fiber任务
    task->resume();

    if (task->getState() == FiberState::SUSPENDED) {
        // if not blocked
        auto& scheduler = Scheduler::GetScheduler();
        scheduler.scheduleImmediate(task);
    }
    // 如果状态是DONE，fiber已完成，task的shared_ptr会自动释放
}

} // namespace fiber