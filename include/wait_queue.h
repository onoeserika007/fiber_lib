#ifndef FIBER_WAIT_QUEUE_H
#define FIBER_WAIT_QUEUE_H

#include <memory>
#include <atomic>
#include <vector>
#include "fiber.h"
#include "lockfree_linked_queue.h"

namespace fiber {

/**
 * @brief 无锁协程等待队列
 * 
 * 使用无锁链表管理等待某个条件的协程队列，提供统一的挂起/唤醒机制
 * 这是所有同步原语（Channel、Mutex、Condition等）的基础
 */
class WaitQueue {
public:
    WaitQueue() = default;
    ~WaitQueue() = default;
    
    // 禁用拷贝和移动
    WaitQueue(const WaitQueue&) = delete;
    WaitQueue& operator=(const WaitQueue&) = delete;
    WaitQueue(WaitQueue&&) = delete;
    WaitQueue& operator=(WaitQueue&&) = delete;
    
    /**
     * @brief 将当前协程加入等待队列并挂起
     * 
     * 调用此方法的协程会被挂起，直到被notify唤醒
     * 必须在协程上下文中调用
     */
    void wait();
    
    /**
     * @brief 唤醒一个等待的协程
     * 
     * @return 是否成功唤醒了协程
     */
    bool notify_one();
    
    /**
     * @brief 唤醒所有等待的协程
     * 
     * @return 被唤醒的协程数量
     */
    size_t notify_all();
    
    /**
     * @brief 检查是否为空（非线程安全，仅用于调试）
     */
    bool empty() const;
    
    void push_back_lockfree(Fiber::ptr fiber);

private:
    
    // 从队列头部取出节点
    Fiber::ptr pop_front_lockfree();

    LockFreeLinkedList<Fiber::ptr> lock_free_queue_;
};

} // namespace fiber

#endif // FIBER_WAIT_QUEUE_H