#include "fiber_consumer.h"
#include "scheduler.h"
#include "logger.h"
#include <iostream>
#include <thread>
#include <chrono>

namespace fiber {

FiberConsumer::FiberConsumer(int id, Scheduler* scheduler) 
    : id_(id)
    , scheduler_(scheduler)
    , queue_(std::make_unique<LockFreeQueue<std::shared_ptr<Fiber>>>(QUEUE_SIZE)) {
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

    Fiber::ptr task;
    while (queue_->try_pop(task)) {
        // 可选：标记任务取消状态，或者直接 resume 完成
        // task->cancel(); 或 task->resume();
    }

    // 这里一直报错是因为我在用FiberConsumer线程自己join自己，当然会出错了
    if (thread_.joinable()) {
        thread_.join();
    }
}

void FiberConsumer::schedule(Fiber::ptr fiber) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    
    // 尝试push到lock-free队列，如果队列满了就自旋等待
    while (!queue_->try_push(fiber)) {
        // 队列满，短暂等待后重试
        std::this_thread::yield();
    }
}

size_t FiberConsumer::getQueueSize() const {
    return queue_->size();
}

void FiberConsumer::consumerLoop() {
    LOG_DEBUG("FiberConsumer {} started", id_);
    
    while (running_.load(std::memory_order_acquire)) {
        processTask();
    }
    
    LOG_DEBUG("FiberConsumer {} stopped", id_);
}

void FiberConsumer::processTask() {
    // Lock-free地从队列获取任务
    Fiber::ptr task;
    if (!queue_->try_pop(task)) {
        // 队列为空，短暂休眠避免busy-wait
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        return;
    }
    
    if (!task) {
        return;
    }
    
    // 执行fiber任务
    task->resume();

    if (task->getState() == FiberState::SUSPENDED) {
        // if not blocked
        while (!queue_->try_push(std::move(task))) {
            // 队列满了，短暂等待
            std::this_thread::yield();
        }
    }
    // 如果状态是DONE，fiber已完成，task的shared_ptr会自动释放
}

} // namespace fiber