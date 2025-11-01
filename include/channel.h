#ifndef FIBER_CHANNEL_LOCKFREE_H
#define FIBER_CHANNEL_LOCKFREE_H

#include <memory>
#include <atomic>
#include <vector>
#include "fiber.h"
#include "wait_queue.h"

namespace fiber {

// 前置声明
class WaitQueue;
class SelectCase;

/**
 * @brief 无锁协程Channel类
 */
template<typename T>
class Channel {
public:
    using value_type = T;
    using ptr = std::shared_ptr<Channel<T>>;
    
    explicit Channel(size_t capacity = 0);
    ~Channel();
    
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    
    // 阻塞发送和接收
    bool send(T value);
    bool recv(T& value);
    
    // 非阻塞发送和接收  
    bool try_send(T value);
    bool try_recv(T& value);
    
    // Channel管理
    void close();
    bool is_closed() const;
    size_t size() const;
    size_t capacity() const;
    bool empty() const;
    bool full() const;

private:
    friend class SelectCase;
    
    enum class State { OPEN, CLOSED };
    
    // 无锁环形缓冲区
    struct alignas(64) Slot {
        std::atomic<T*> data{nullptr};
    };
    
    size_t capacity_;
    std::vector<Slot> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};  
    alignas(64) std::atomic<State> state_{State::OPEN};
    
    std::unique_ptr<WaitQueue> send_waiters_;
    std::unique_ptr<WaitQueue> recv_waiters_;
    
    // 辅助方法
    size_t next_index(size_t current) const { return (current + 1) % capacity_; }
    bool is_full_lockfree() const;
    bool is_empty_lockfree() const;
    bool try_push_lockfree(T&& value);
    bool try_pop_lockfree(T& value);
};

// 实现
template<typename T>
Channel<T>::Channel(size_t capacity) 
    : capacity_(capacity == 0 ? 2 : capacity + 1),
      buffer_(capacity_),
      send_waiters_(std::make_unique<WaitQueue>()),
      recv_waiters_(std::make_unique<WaitQueue>()) {
}

template<typename T>
Channel<T>::~Channel() {
    close();
}

template<typename T>
bool Channel<T>::send(T value) {
    if (state_.load(std::memory_order_acquire) == State::CLOSED) {
        return false;
    }
    
    if (try_push_lockfree(std::move(value))) {
        recv_waiters_->notify_one();
        return true;
    }
    
    while (true) {
        // yield cpu
        send_waiters_->wait();
        
        if (state_.load(std::memory_order_acquire) == State::CLOSED) {
            return false;
        }
        
        if (try_push_lockfree(std::move(value))) {
            recv_waiters_->notify_one();
            return true;
        }
    }
}

template<typename T>
bool Channel<T>::recv(T& value) {
    if (try_pop_lockfree(value)) {
        send_waiters_->notify_one();
        return true;
    }
    
    if (state_.load(std::memory_order_acquire) == State::CLOSED && is_empty_lockfree()) {
        return false;
    }
    
    while (true) {
        recv_waiters_->wait();
        
        if (try_pop_lockfree(value)) {
            send_waiters_->notify_one();
            return true;
        }
        
        if (state_.load(std::memory_order_acquire) == State::CLOSED && is_empty_lockfree()) {
            return false;
        }
    }
}

template<typename T>
bool Channel<T>::try_send(T value) {
    if (state_.load(std::memory_order_relaxed) == State::CLOSED) {
        return false;
    }
    
    if (try_push_lockfree(std::move(value))) {
        recv_waiters_->notify_one();
        return true;
    }
    
    return false;
}

template<typename T>
bool Channel<T>::try_recv(T& value) {
    if (try_pop_lockfree(value)) {
        send_waiters_->notify_one();
        return true;
    }
    return false;
}

template<typename T>
void Channel<T>::close() {
    state_.store(State::CLOSED, std::memory_order_release);
    send_waiters_->notify_all();
    recv_waiters_->notify_all();
}

template<typename T>
bool Channel<T>::is_closed() const {
    return state_.load(std::memory_order_acquire) == State::CLOSED;
}

template<typename T>
size_t Channel<T>::size() const {
    size_t h = head_.load(std::memory_order_relaxed);
    size_t t = tail_.load(std::memory_order_relaxed);
    return t >= h ? (t - h) : (capacity_ - h + t);
}

template<typename T>
size_t Channel<T>::capacity() const {
    return capacity_ - 1;  // -1 because ring buffer needs one empty slot
}

template<typename T>
bool Channel<T>::empty() const {
    return is_empty_lockfree();
}

template<typename T>
bool Channel<T>::full() const {
    return is_full_lockfree();
}

// 私有辅助方法
template<typename T>
bool Channel<T>::is_full_lockfree() const {
    size_t current_head = head_.load(std::memory_order_acquire);
    size_t current_tail = tail_.load(std::memory_order_acquire);
    return next_index(current_tail) == current_head;
}

template<typename T>
bool Channel<T>::is_empty_lockfree() const {
    size_t current_head = head_.load(std::memory_order_acquire);
    size_t current_tail = tail_.load(std::memory_order_acquire);
    return current_head == current_tail;
}

template<typename T>
bool Channel<T>::try_push_lockfree(T&& value) {
    size_t current_tail = tail_.load(std::memory_order_relaxed);
    size_t next_tail = next_index(current_tail);
    
    if (next_tail == head_.load(std::memory_order_acquire)) {
        return false;  // 满了
    }
    
    T* new_data = new T(std::move(value));
    T* expected = nullptr;
    
    if (buffer_[current_tail].data.compare_exchange_weak(
            expected, new_data, 
            std::memory_order_release, 
            std::memory_order_relaxed)) {
        
        tail_.store(next_tail, std::memory_order_release);
        return true;
    } else {
        delete new_data;
        return false;
    }
}

template<typename T>
bool Channel<T>::try_pop_lockfree(T& value) {
    size_t current_head = head_.load(std::memory_order_relaxed);
    
    if (current_head == tail_.load(std::memory_order_acquire)) {
        return false;  // 空了
    }
    
    T* data = buffer_[current_head].data.exchange(nullptr, std::memory_order_acquire);
    if (data != nullptr) {
        value = std::move(*data);
        delete data;
        head_.store(next_index(current_head), std::memory_order_release);
        return true;
    }
    
    return false;
}

// 便利函数
template<typename T>
typename Channel<T>::ptr make_channel(size_t capacity = 0) {
    return std::make_shared<Channel<T>>(capacity);
}

} // namespace fiber

#endif // FIBER_CHANNEL_LOCKFREE_H