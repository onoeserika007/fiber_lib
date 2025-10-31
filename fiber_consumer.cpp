#include "fiber_consumer.h"
#include "scheduler.h"
#include <iostream>

namespace fiber {

FiberConsumer::FiberConsumer(int id, Scheduler* scheduler) 
    : id_(id), scheduler_(scheduler) {
}

FiberConsumer::~FiberConsumer() {
    stop();
}

void FiberConsumer::start() {
    if (running_.exchange(true)) {
        return; // 已经在运行
    }
    
    thread_ = std::thread(&FiberConsumer::consumerLoop, this);
}

void FiberConsumer::stop() {
    if (!running_.exchange(false)) {
        return; // 已经停止
    }
    
    {
        std::lock_guard<std::mutex> lock(local_mutex_);
        local_cv_.notify_one();
    }
    
    if (thread_.joinable()) {
        thread_.join();
    }
}

void FiberConsumer::schedule(Fiber::ptr fiber) {
    if (!running_.load()) {
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(local_mutex_);
        local_queue_.push(fiber);
    }
    local_cv_.notify_one();
}

size_t FiberConsumer::getQueueSize() const {
    std::lock_guard<std::mutex> lock(local_mutex_);
    return local_queue_.size();
}

void FiberConsumer::consumerLoop() {
    std::cout << "FiberConsumer " << id_ << " started" << std::endl;
    
    while (running_.load()) {
        processTask();
    }
    
    std::cout << "FiberConsumer " << id_ << " stopped" << std::endl;
}

void FiberConsumer::processTask() {
    Fiber::ptr task;
    
    {
        std::unique_lock<std::mutex> lock(local_mutex_);
        local_cv_.wait(lock, [this]() {
            return !local_queue_.empty() || !running_.load();
        });
        
        if (!running_.load()) {
            return;
        }
        
        if (!local_queue_.empty()) {
            task = local_queue_.front();
            local_queue_.pop();
        }
    }
    
    if (task) {
        // 执行fiber任务
        task->resume();
    }
}

} // namespace fiber