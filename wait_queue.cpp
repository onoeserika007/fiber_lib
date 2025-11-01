#include "include/wait_queue.h"
#include "include/scheduler.h"
#include <cassert>
#include <stdexcept>
#include <iostream>

namespace fiber {

WaitQueue::~WaitQueue() {
    // 清理剩余的节点
    WaitNode* current = head_.load();
    while (current) {
        WaitNode* next = current->next.load();
        delete current;
        current = next;
    }
}

// 入队前，检查ready earlyreturn即可；入队后，把block yield改成普通yield，notify检查测只对block的fiber生效
// 可能会有竞态问题，暂时先不管了，以后再修吧
void WaitQueue::wait() {
    // 获取当前协程的shared_ptr
    auto current_fiber = Fiber::GetCurrentFiberPtr();
    if (!current_fiber) {
        std::cerr << "ERROR: wait() called outside of fiber context" << std::endl;
        throw std::runtime_error("wait() must be called from within a fiber");
    }


    // 无锁添加到等待队列
    push_back_lockfree(current_fiber);
    
    // std::cout << "DEBUG: Fiber " << current_fiber->getId() << " entering wait queue (lockfree)" << std::endl;
    
    // 让出执行权，等待被唤醒
    Fiber::block_yield();
    
    // std::cout << "DEBUG: Fiber " << current_fiber->getId() << " resumed from wait queue (lockfree)" << std::endl;
}

bool WaitQueue::notify_one() {
    // 无锁从队列头部取出一个协程
    auto fiber = pop_front_lockfree();

    if (!fiber) {
        return false;  // 队列为空
    }

    // 获取调度器并重新调度协程
    auto&& scheduler = Scheduler::GetScheduler();
    scheduler.scheduleImmediate(fiber);
    // std::cout << "DEBUG: Notified and rescheduled waiting fiber (lockfree)" << std::endl;

    return true;
}

std::size_t WaitQueue::notify_all() {
    std::size_t count = 0;
    auto&& scheduler = Scheduler::GetScheduler();

    // 持续取出所有等待的协程
    while (auto fiber = pop_front_lockfree()) {
        scheduler.scheduleImmediate(fiber);
        count++;
        // std::cout << "DEBUG: Notified waiting fiber (notify_all, lockfree)" << std::endl;
    }

    return count;
}

void WaitQueue::push_back_lockfree(Fiber::ptr fiber) {
    WaitNode* new_node = new WaitNode(fiber);
    
    // 使用CAS操作无锁插入到链表尾部
    WaitNode* prev_tail = tail_.exchange(new_node);
    
    if (prev_tail == nullptr) {
        // 队列原本为空，同时设置head
        head_.store(new_node);
    } else {
        // 链接到前一个tail节点
        prev_tail->next.store(new_node);
    }
}

Fiber::ptr WaitQueue::pop_front_lockfree() {
    WaitNode* head = head_.load();
    
    // 队列为空
    if (head == nullptr) {
        return nullptr;
    }
    
    // 尝试使用CAS更新head指针
    WaitNode* next = head->next.load();
    
    if (!head_.compare_exchange_weak(head, next)) {
        // CAS失败，可能被其他线程修改了，返回空
        return nullptr;
    }
    
    // 如果这是最后一个节点，同时更新tail
    if (next == nullptr) {
        WaitNode* expected_tail = head;
        tail_.compare_exchange_weak(expected_tail, nullptr);
    }
    
    Fiber::ptr result = head->fiber;
    delete head;
    return result;
}

bool WaitQueue::empty() const {
    return head_.load() == nullptr;
}

} // namespace fiber