#include "fiber.h"
#include "context.h"
#include "scheduler.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdlib.h>
#include <ucontext.h>

namespace fiber {

thread_local Fiber* Fiber::current_fiber_ = nullptr;
std::atomic<uint64_t> fiber_id_counter{0};

uint64_t Fiber::generateId() {
    return ++fiber_id_counter;
}

Fiber::Fiber(FiberFunction func) 
    : id_(generateId()), 
      state_(FiberState::READY),
      func_(std::move(func)),
      run_mode_(RunMode::MANUAL) {
      
    if (!func_) {
        current_fiber_ = this;
        state_ = FiberState::RUNNING;
        context_.reset(ContextFactory::createContext());
        return;
    }
    
    context_.reset(ContextFactory::createContext());
    context_->initialize(&Fiber::fiberEntry);
    
    std::cout << "Fiber created with ID: " << id_ << std::endl;
}

Fiber::~Fiber() {
    std::cout << "Fiber destroyed with ID: " << id_ << std::endl;
}

void Fiber::resume() {
    assert(state_ != FiberState::DONE && "Cannot resume a finished fiber");
    assert(context_ && "Fiber context is null");
    
    if (state_ == FiberState::READY || state_ == FiberState::SUSPENDED) {
        Fiber* old_fiber = current_fiber_;
        
        std::cout << "Resuming fiber " << id_ << std::endl;
        
        if (old_fiber && old_fiber->context_) {
            current_fiber_ = this;
            state_ = FiberState::RUNNING;
            old_fiber->context_->switchTo(context_.get());
        } else {
            // 通过调度器获取main_fiber
            auto main_fiber = Scheduler::GetMainFiber();
            current_fiber_ = this;
            state_ = FiberState::RUNNING;
            main_fiber->context_->switchTo(context_.get());
        }
    }
}



void Fiber::yield() {
    assert(current_fiber_ && "No current fiber");
    
    Fiber* current = current_fiber_;
    
    // 只有在不是DONE状态时才设置为SUSPENDED
    if (current->state_ != FiberState::DONE) {
        current->state_ = FiberState::SUSPENDED;
    }
    
    std::cout << "Yielding fiber " << current->id_ << std::endl;
    
    // 通过 Scheduler 获取 main_fiber，多线程安全
    auto main_fiber = Scheduler::GetMainFiber();
    assert(main_fiber && "main_fiber must be not null");
    current_fiber_ = main_fiber.get();
    main_fiber->state_ = FiberState::RUNNING;
    current->context_->switchTo(main_fiber->context_.get());
}

FiberState Fiber::getState() const {
    return state_;
}

uint64_t Fiber::getId() const {
    return id_;
}

void Fiber::YieldToScheduler() {
    yield();
}

void Fiber::fiberEntry() {
    assert(current_fiber_ && "No current fiber in fiberEntry");
    
    if (current_fiber_->func_) {
        current_fiber_->func_();
    }
    
    current_fiber_->state_ = FiberState::DONE;
    
    std::cout << "Fiber " << current_fiber_->id_ << " finished" << std::endl;
    yield();
}

// =========================
// Go语义接口实现
// =========================

void Fiber::go(FiberFunction func) {
    auto fiber = std::make_shared<Fiber>(std::move(func));
    fiber->setRunMode(RunMode::SCHEDULED);
    
    // 获取多线程调度器并立即调度
    auto scheduler = Scheduler::GetOrCreateMultiThreadScheduler();
    scheduler->scheduleImmediate(fiber);
}

int Fiber::getWorkerCount() {
    auto scheduler = Scheduler::GetScheduler();
    return scheduler ? scheduler->getWorkerCount() : 0;
}

} // namespace fiber