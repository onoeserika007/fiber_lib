#include "fiber.h"
#include "context.h"
#include "scheduler.h"
#include "logger.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdlib.h>
#include <ucontext.h>

namespace fiber {

thread_local Fiber* Fiber::current_fiber_ = nullptr;
thread_local std::weak_ptr<Fiber> Fiber::current_fiber_weak_;
std::atomic<uint64_t> fiber_id_counter{0};

uint64_t Fiber::generateId() {
    return ++fiber_id_counter;
}



Fiber::Fiber(FiberFunction func) 
    : id_(generateId()), 
      state_(FiberState::READY),
      func_(std::move(func)),
      run_mode_(RunMode::MANUAL) {

}

void Fiber::Init() {
    // main fiber
    if (!func_) {
        auto self = shared_from_this();
        SetCurrentFiberPtr(self);
        state_ = FiberState::RUNNING;
        context_.reset(ContextFactory::createContext());
        return;
    }

    // non-main fiber 需要先初始化一片上下文以供切换
    context_.reset(ContextFactory::createContext());
    context_->initialize(&Fiber::fiberEntry);

    // LOG_DEBUG("Fiber created with ID: {}", id_);  // 过于频繁，注释掉
}

Fiber::~Fiber() {
    // LOG_DEBUG("Fiber destroyed with ID: {}", id_);  // 过于频繁，注释掉
}

void Fiber::resume() {
    assert(state_ != FiberState::DONE && "Cannot resume a finished fiber");
    assert(context_ && "Fiber context is null");
    
    if (state_ == FiberState::READY || state_ == FiberState::SUSPENDED) {
        Fiber* old_fiber = current_fiber_;
        
        // LOG_DEBUG("Resuming fiber {}", id_);  // 过于频繁，注释掉
        
        if (old_fiber && old_fiber->context_) {
            SetCurrentFiberPtr(shared_from_this());
            // 注意：这里我们没有shared_ptr来设置weak_ptr，这是一个问题
            state_ = FiberState::RUNNING;
            old_fiber->context_->switchTo(context_.get());
        } else {
            // 通过调度器获取main_fiber
            auto main_fiber = Scheduler::GetMainFiber();
            SetCurrentFiberPtr(shared_from_this());
            // 注意：这里我们没有shared_ptr来设置weak_ptr，这是一个问题
            state_ = FiberState::RUNNING;
            main_fiber->context_->switchTo(context_.get());
        }
    }
}



void Fiber::yield() {
    Fiber* current = current_fiber_;
    assert(current && "No current fiber");
    
    // 只有在不是DONE状态时才设置为SUSPENDED
    if (current->state_ != FiberState::DONE) {
        current->state_ = FiberState::SUSPENDED;
    }
    
    // LOG_DEBUG("Yielding fiber {}", current->id_);  // 过于频繁，注释掉
    
    // 通过 Scheduler 获取 main_fiber，多线程安全
    auto main_fiber = Scheduler::GetMainFiber();
    assert(main_fiber && "main_fiber must be not null");
    SetCurrentFiberPtr(main_fiber);
    main_fiber->state_ = FiberState::RUNNING;
    current->context_->switchTo(main_fiber->context_.get());
}

FiberState Fiber::getState() const {
    return state_;
}

uint64_t Fiber::getId() const {
    return id_;
}

void Fiber::fiberEntry() {
    auto current = current_fiber_;
    assert(current && "No current fiber in fiberEntry");
    
    if (current->func_) {
        current->func_();
    }
    
    current->state_ = FiberState::DONE;

    yield();
}

// =========================
// Go语义接口实现
// =========================

void Fiber::go(FiberFunction func) {
    auto fiber = Fiber::create(std::move(func));
    fiber->setRunMode(RunMode::SCHEDULED);
    
    // 获取多线程调度器并立即调度
    auto scheduler = Scheduler::GetOrCreateMultiThreadScheduler();
    scheduler->scheduleImmediate(fiber);
}

int Fiber::getWorkerCount() {
    auto scheduler = Scheduler::GetScheduler();
    return scheduler ? scheduler->getWorkerCount() : 0;
}

Fiber::ptr Fiber::GetCurrentFiberPtr() {
    return current_fiber_weak_.lock();  // 安全地转换为shared_ptr
}

void Fiber::SetCurrentFiberPtr(const ptr& fiber) {
    current_fiber_ = fiber.get();      // 设置裸指针用于快速访问
    current_fiber_weak_ = fiber;       // 设置weak_ptr用于安全获取shared_ptr
}

} // namespace fiber