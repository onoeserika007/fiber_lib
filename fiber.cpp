#include "fiber.h"
#include "context.h"
#include "scheduler.h"
#include "timer.h"
#include "logger.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdlib.h>
#include <ucontext.h>

namespace fiber {

thread_local Fiber::ptr Fiber::main_fiber_;
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
    assert(state_ == FiberState::DONE && "destroying a non-finished fiber");
}

void Fiber::resume() {
    assert(state_ != FiberState::DONE && "Cannot resume a finished fiber");
    assert(context_ && "Fiber context is null");
    
    auto current_fiber = Fiber::GetCurrentFiberPtr();
    if (!current_fiber) {
        current_fiber = GetMainFiber();
        // LOG_DEBUG("[resume] Building main fiber {}", current_fiber->getId());
        SetCurrentFiberPtr(current_fiber);
    }
    parent_fiber_ = current_fiber;

    assert(parent_fiber_ && parent_fiber_->context_ && "parent_fiber_ incomplete");
    // LOG_DEBUG("[resume] Resuming from parent {} to {}", parent_fiber_->getId(), getId());

    SetCurrentFiberPtr(shared_from_this());
    state_ = FiberState::RUNNING;
    parent_fiber_->context_->switchTo(context_.get());
}

void Fiber::yield() {
    Fiber* current = current_fiber_;
    assert(current && "No current fiber");
    
    // 只有在不是DONE状态时才设置为SUSPENDED
    if (current->state_ != FiberState::DONE) {
        current->state_ = FiberState::SUSPENDED;
    }

    yield_internal(current);
}

void Fiber::block_yield() {
    Fiber* current = current_fiber_;
    assert(current && "No current fiber");

    // 只有在不是DONE状态时才设置为SUSPENDED
    if (current->state_ != FiberState::DONE) {
        current->state_ = FiberState::BLOCKED;
    }

    yield_internal(current);
}

void Fiber::yield_internal(Fiber *current) {
    // 只有为DONE状态时才清除parent ptr
    auto parent_fiber = current->parent_fiber_;
    if (current->state_ == FiberState::DONE) {
        current->parent_fiber_ = nullptr;
    }

    // LOG_DEBUG("[do_yield] yielding from {}", current->getId());
    assert(parent_fiber && "parent_fiber must be not null");
    SetCurrentFiberPtr(parent_fiber);
    parent_fiber->state_ = FiberState::RUNNING;
    current->context_->switchTo(parent_fiber->context_.get());
}

FiberState Fiber::getState() const {
    return state_;
}

void Fiber::setState(FiberState state) {
    state_ = state;
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

    yield_internal(current);
}

Fiber::ptr Fiber::GetMainFiber() {
    if (!main_fiber_) {
        main_fiber_ = Fiber::create(Fiber::FiberFunction{});
    }
    return main_fiber_;
}

// =========================
// Go语义接口实现
// =========================

void Fiber::go(FiberFunction func) {
    auto fiber = Fiber::create(std::move(func));
    fiber->setRunMode(RunMode::SCHEDULED);
    
    // 获取多线程调度器并立即调度
    auto&& scheduler = Scheduler::GetScheduler();
    scheduler.scheduleImmediate(fiber);
}

int Fiber::getWorkerCount() {
    auto&& scheduler = Scheduler::GetScheduler();
    return scheduler.getWorkerCount();
}

void Fiber::sleep(uint64_t ms) {
    if (ms == 0) {
        // 0毫秒，直接返回，不做任何操作
        return;
    }

    auto current = GetCurrentFiberPtr();
    if (!current) {
        // 不在协程中，使用std::this_thread::sleep_for
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return;
    }

    auto& timer_wheel = TimerWheel::getInstance();
    timer_wheel.addTimer(ms, [current]() {
        // 定时器到期，重新调度该协程
        auto&& scheduler = Scheduler::GetScheduler();
        scheduler.scheduleImmediate(current);
    }, false);

    Fiber::block_yield();
}

Fiber::ptr Fiber::GetCurrentFiberPtr() {
    return current_fiber_weak_.lock();  // 安全地转换为shared_ptr
}

void Fiber::SetCurrentFiberPtr(const ptr& fiber) {
    current_fiber_ = fiber.get();      // 设置裸指针用于快速访问
    current_fiber_weak_ = fiber;       // 设置weak_ptr用于安全获取shared_ptr
}

} // namespace fiber