//
// Created by inory on 11/12/25.
//

#ifndef LOCKFREE_LINKED_QUEUE_H
#define LOCKFREE_LINKED_QUEUE_H
#include <atomic>
#include <optional>

namespace fiber {

/**
* @brief 协程等待队列节点
*/
template<typename T>
struct WaitNode {
    T data;
    std::atomic<WaitNode*> next{nullptr};
    std::atomic<int> refcnt{1};  // 引用计数，初始为 1（自持有）
    WaitNode(): data() {}

    template<typename Y>
    explicit WaitNode(Y&& _data) : data(std::forward<Y>(_data)) {}

    // 引用计数管理
    void retain() noexcept {
        refcnt.fetch_add(1, std::memory_order_acq_rel);
    }

    void release() noexcept {
        if (refcnt.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

/**
 * @brief 无锁协程等待队列
 *
 * 使用无锁链表管理等待某个条件的协程队列，提供统一的挂起/唤醒机制
 * 这是所有同步原语（Channel、Mutex、Condition等）的基础
 */
template <typename T>
class LockFreeLinkedList {
public:
    using Node = WaitNode<T>;
    LockFreeLinkedList() : head_(nullptr), tail_(nullptr) {
        auto dummy = new Node;
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }

    ~LockFreeLinkedList() {
        // 清理剩余的节点
        Node* cur = safe_read(head_);
        while (cur) {
            Node* next = cur->next.load(std::memory_order_relaxed);
            cur->release(); // release temp
            cur->release(); // real delete
            cur = next ? safe_read(next) : nullptr;
        }
    }

    // 禁用拷贝和移动
    LockFreeLinkedList(const LockFreeLinkedList&) = delete;
    LockFreeLinkedList& operator=(const LockFreeLinkedList&) = delete;
    LockFreeLinkedList(LockFreeLinkedList&&) = delete;
    LockFreeLinkedList& operator=(LockFreeLinkedList&&) = delete;

    /**
     * @brief 检查是否为空（非线程安全，仅用于调试）
     */
    bool empty() const {
        Node* h = safe_read(head_);
        Node* t = safe_read(tail_);
        bool res = (h == t) && (h->next.load(std::memory_order_acquire) == nullptr);
        h->release();  // 释放临时引用
        t->release();
        return res;
    }


    static Node* safe_read(const std::atomic<Node*>& ptr) {
        while (true) {
            Node* p = ptr.load(std::memory_order_acquire);
            if (p == nullptr) return nullptr;
            p->retain();  // ref++
            // refcnt方案重点在于防止错误释放的节点产生，而根据内容保证一致性
            // 所以这里也可以直接替换成只能指针，如果不在意性能的话
            if (p == ptr.load(std::memory_order_relaxed )) {
                return p; // successfully get snapshot
            }
            p->release(); // ref--
        }
    }

    // std::atomic_compare_exchange_weak
    // "If the comparison fails, the value of expected is updated to the value held by the atomic object at the time of the failure."
    template <typename Y>
    void push_back_lockfree(Y&& _data) {
        Node* new_node = new Node(std::forward<Y>(_data));

        while (true) {
            // Take tail and tail-next snapshot
            Node* tail = safe_read(tail_);
            Node* tail_orig = tail;
            Node* next = tail->next.load(std::memory_order_acquire);

            // continue if tail has changed
            if (tail != tail_) {
                tail_orig->release();
                continue;
            }

            // step global tail
            if (next != nullptr) {
                // they do the same thing
                // if compare_exchange_weak failed, tail will be updated to the real value of tail_
                tail_.compare_exchange_weak(tail, next, std::memory_order_release, std::memory_order_relaxed);
                tail_orig->release();
                continue;
            }

            // if insert new node success, break
            if (tail->next.compare_exchange_weak(next, new_node, std::memory_order_release, std::memory_order_relaxed)) {
                // they do the same thing
                // step global tail
                tail_.compare_exchange_weak(tail, new_node, std::memory_order_release, std::memory_order_relaxed);
                tail_orig->release();
                break;
            }

            tail_orig->release();
        }

    }

    // 从队列头部取出节点
    std::optional<T> pop_front_lockfree() {
        T result;

        while (true) {
            Node* head = safe_read(head_);
            Node* tail = safe_read(tail_);
            Node* head_orig = head;
            Node* tail_orig = tail;
            Node* next = head->next.load(std::memory_order_acquire);

            // head_ has moved, retry
            if (head != head_.load(std::memory_order_acquire)) {
                head_orig->release();
                tail_orig->release();
                continue;
            }

            // empty
            if (head == tail && next == nullptr) {
                head_orig->release();
                tail_orig->release();
                return {};
            }

            // if tail_ lagged off to real tail
            if (head == tail && next != nullptr) {
                tail_.compare_exchange_weak(tail, next, std::memory_order_release, std::memory_order_relaxed);
                head_orig->release();
                tail_orig->release();
                continue;
            }

            // Normally dequeue, trying to move head
            if (head_.compare_exchange_weak(head, next, std::memory_order_release, std::memory_order_relaxed)) {
                result = std::move(next->data);
                head_orig->release();
                // real delete
                head_orig->release();
                tail_orig->release();
                break;
            }

            head_orig->release();
            tail_orig->release();
        }

        return { std::move(result) };
    }

private:
    // 使用无锁链表实现等待队列
    alignas(64) std::atomic<Node*> head_;
    alignas(64) std::atomic<Node*> tail_;

};

}

#endif //LOCKFREE_LINKED_QUEUE_H