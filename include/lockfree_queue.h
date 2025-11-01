#ifndef FIBER_LOCKFREE_QUEUE_H
#define FIBER_LOCKFREE_QUEUE_H

#include <atomic>
#include <vector>
#include <memory>
#include <new>
#include <type_traits>

namespace fiber {

/**
 * @brief 真·无锁环形队列（Lock-Free Ring Buffer）
 *
 * - 使用固定容量预分配存储（in-place placement new）
 * - 仅通过原子操作维护 head/tail 索引
 * - 无任何 new/delete（避免堆锁）
 * - 支持任意可移动类型 T
 * - 支持多线程单生产者/单消费者（SPMC/MPMC 需额外扩展）
 */
template<typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t capacity)
        : capacity_(capacity + 1), buffer_(capacity_) {}

    ~LockFreeQueue() {
        T tmp;
        while (try_pop(tmp)) {}  // 清理残留
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    /**
     * @brief 尝试推入一个元素（非阻塞）
     */
    bool try_push(T&& value) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = next_index(tail);

        // 检查队列是否满
        if (next == head_.load(std::memory_order_acquire))
            return false;

        Slot& slot = buffer_[tail];
        bool expected = false;

        // 尝试占用该槽位
        if (!slot.occupied.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_relaxed))
            return false;

        // 在槽内原地构造对象（placement new）
        new (&slot.storage) T(std::move(value));

        // 推进尾指针
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool try_push(const T& value) {
        T copy = value;
        return try_push(std::move(copy));
    }

    /**
     * @brief 尝试弹出一个元素（非阻塞）
     */
    bool try_pop(T& out_value) {
        size_t head = head_.load(std::memory_order_relaxed);

        // 检查是否为空
        if (head == tail_.load(std::memory_order_acquire))
            return false;

        Slot& slot = buffer_[head];

        if (!slot.occupied.load(std::memory_order_acquire))
            return false;

        // 从槽内取出值并析构
        T* ptr = reinterpret_cast<T*>(&slot.storage);
        out_value = std::move(*ptr);
        ptr->~T();

        slot.occupied.store(false, std::memory_order_release);

        // 推进头指针
        head_.store(next_index(head), std::memory_order_release);
        return true;
    }

    /**
     * @brief 获取当前队列中元素数量（近似）
     */
    size_t size() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (t >= h) ? (t - h) : (capacity_ - h + t);
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    bool full() const {
        size_t next = next_index(tail_.load(std::memory_order_acquire));
        return next == head_.load(std::memory_order_acquire);
    }

    size_t capacity() const { return capacity_ - 1; }

private:
    struct alignas(64) Slot {
        std::atomic<bool> occupied{false};
        // 分配一块原始字节存储区域，它的大小能装下 T，并且满足 T 的对齐要求。
        typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
    };

    size_t next_index(size_t i) const noexcept {
        return (i + 1) % capacity_;
    }

    const size_t capacity_;
    std::vector<Slot> buffer_;

    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace fiber

#endif // FIBER_LOCKFREE_QUEUE_H
