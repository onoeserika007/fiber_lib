//
// Created by inory on 11/12/25.
//

#ifndef LOCKFREE_LINKED_QUEUE_H
#define LOCKFREE_LINKED_QUEUE_H

#include <atomic>
#include <optional>

#include "freelist.h"
#include "lockfree/tagged_node_ptr.h"

#define FIBER_LIKELY(x) __builtin_expect(x, 1)
#define FIBER_UNLIKELY(x) __builtin_expect(x, 0)

/*
 * ABA解决方案，废弃原refcnt方案，需要搭配hazard pointer使用
 * 现在使用boost的tagged pointer方案
 */

namespace fiber {

/**
 * @brief 无锁协程等待队列
 *
 * 使用无锁链表管理等待某个条件的协程队列，提供统一的挂起/唤醒机制
 * 这是所有同步原语（Channel、Mutex、Condition等）的基础
 */
template<typename T>
class LockFreeLinkedList {


    struct ListNode {
        alignas(64) T data; // 消费者访问
        char pad1[64 - sizeof(T)]{}; // 填充

        alignas(64) std::atomic<TaggedPtr<ListNode>> next{}; // 生产者访问
        char pad2[64 - sizeof(std::atomic<TaggedPtr<ListNode>>)]{}; // 填充

        ListNode() : data() {}

        template<typename Y, std::enable_if_t<!std::is_same_v<std::decay_t<Y>, ListNode *>, int> = 0>
        explicit ListNode(Y &&_data) : data(std::forward<Y>(_data)) {}

        template<typename Y>
        ListNode(Y &&_data, ListNode *_next) : data(std::forward<Y>(_data)), next(TaggedPtr<ListNode>(_next)) {}

        explicit ListNode(ListNode *_next) : next(TaggedPtr<ListNode>(_next)) {}
    };

    using Node = ListNode;
    using TaggedNodePtr = TaggedPtr<Node>;
    using PoolType = FreeList<Node>;

public:
    LockFreeLinkedList() : head_(), tail_(), pool_(std::allocator<Node>()) {
        Node *dummy = pool_.template construct<true, false>(pool_.null_handle());
        TaggedNodePtr dummy_node{dummy};
        head_.store(dummy_node, std::memory_order_release);
        tail_.store(dummy_node, std::memory_order_release);
    }

    ~LockFreeLinkedList() {
        // 清理剩余的节点
        while (!empty()) {
            pop_front_lockfree();
        }
    }

    // 禁用拷贝和移动
    LockFreeLinkedList(const LockFreeLinkedList &) = delete;
    LockFreeLinkedList &operator=(const LockFreeLinkedList &) = delete;
    LockFreeLinkedList(LockFreeLinkedList &&) = delete;
    LockFreeLinkedList &operator=(LockFreeLinkedList &&) = delete;

    /**
     * @brief 检查是否为空（非线程安全，仅用于调试）
     */
    bool empty() const {
        Node *h = head_.load(std::memory_order_acquire).get_ptr();
        Node *t = tail_.load(std::memory_order_acquire).get_ptr();
        return h == t;
    }

    size_t size() const { return size_.load(std::memory_order_relaxed); }

    // std::atomic_compare_exchange_weak
    // "If the comparison fails, the value of expected is updated to the value held by the atomic object at the time
    // of the failure."
    template<typename Y>
    void push_back_lockfree(Y _data) {
        Node *new_node = pool_.template construct<true, false>(std::move(_data), pool_.null_handle());

        while (true) {
            // Take tail and tail-next snapshot
            TaggedNodePtr tail = tail_.load(std::memory_order_acquire);
            Node *tail_ptr = tail.get_ptr();

            TaggedNodePtr next = tail_ptr->next.load(std::memory_order_acquire);
            Node *next_ptr = next.get_ptr();

            // continue if tail has changed
            if (FIBER_UNLIKELY(tail != tail_.load(std::memory_order_acquire))) {
                continue;
            }

            // step global tail
            if (next_ptr != nullptr) {
                // they do the same thing
                // if compare_exchange_weak failed, tail will be updated to the real value of tail_
                TaggedNodePtr new_tail{next_ptr, tail.get_next_tag()};
                tail_.compare_exchange_weak(tail, new_tail, std::memory_order_acq_rel, std::memory_order_relaxed);
                continue;
            }

            TaggedNodePtr new_tail_next{new_node, next.get_next_tag()};
            // if insert new node success, break
            if (tail_ptr->next.compare_exchange_weak(next, new_tail_next, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
                size_.fetch_add(1, std::memory_order_relaxed);
                // they do the same thing
                // step global tail
                TaggedNodePtr new_tail{new_node, tail.get_next_tag()};
                tail_.compare_exchange_weak(tail, new_tail, std::memory_order_acq_rel, std::memory_order_relaxed);
                break;
            }
        }
    }

    // 从队列头部取出节点
    std::optional<T> pop_front_lockfree() {
        T result;

        while (true) {
            TaggedNodePtr head = head_.load(std::memory_order_acquire);
            Node *head_ptr = head.get_ptr();
            TaggedNodePtr tail = tail_.load(std::memory_order_acquire);
            Node *tail_ptr = tail.get_ptr();
            TaggedNodePtr next = head_ptr->next.load(std::memory_order_acquire);
            Node *next_ptr = next.get_ptr();

            // head_ has moved, retry
            if (FIBER_UNLIKELY(head != head_.load(std::memory_order_acquire))) {
                continue;
            }

            // empty
            if (head_ptr == tail_ptr && next_ptr == nullptr) {
                return {};
            }

            // if tail_ lagged off to real tail
            if (head_ptr == tail_ptr && next_ptr != nullptr) {
                TaggedNodePtr new_tail{next_ptr, tail.get_next_tag()};
                tail_.compare_exchange_weak(tail, new_tail, std::memory_order_acq_rel, std::memory_order_relaxed);
                continue;
            }

            TaggedNodePtr new_head{next_ptr, head.get_next_tag()};
            // Normally dequeue, trying to move head
            if (head_.compare_exchange_weak(head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                size_.fetch_sub(1, std::memory_order_relaxed);
                result = std::move(next_ptr->data);
                pool_.template destruct<true>(head);
                break;
            }
        }

        return {std::move(result)};
    }

private:
    alignas(64) std::atomic<TaggedNodePtr> head_;
    alignas(64) std::atomic<TaggedNodePtr> tail_;

    // obj pool
    PoolType pool_;

    // size
    std::atomic<size_t> size_{};
};

} // namespace fiber

#endif // LOCKFREE_LINKED_QUEUE_H
