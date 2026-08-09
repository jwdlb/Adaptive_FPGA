// This file provides the one lock-free multi-entry queue used by the normal
// live RTL-to-GPU result path. Exactly one producer (VerilatorWorker) writes
// completed RTL results, and exactly one consumer (GpuWorker) reads them.
//
// The producer reserves a free slot, writes directly into that slot, and then
// publishes it. The consumer can see a slot only after publication, and returns
// it to the producer only after it has finished reading it. No mutex, allocation,
// or extra temporary result copy is needed in the steady-state path.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace market_engine::app {

// A bounded single-producer/single-consumer ring buffer with direct slot access.
// `capacity` is the exact number of values it can hold, rather than capacity - 1.
// The caller must ensure that only one thread uses producer functions and only one
// different thread uses consumer functions.
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(const std::size_t capacity)
        : capacity_(require_positive_capacity(capacity)), storage_(std::make_unique<T[]>(capacity_)) {}

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    // Reserve one writable slot for the producer. A null pointer means every slot
    // is occupied by a published unread value. The producer must call exactly one
    // of publish_push() or cancel_push() before reserving another slot.
    [[nodiscard]] T* try_reserve_push() {
        if (producer_has_reservation_) {
            throw std::logic_error("SPSC producer already has a reserved slot");
        }
        const std::uint64_t consumer_position = released_read_position_.load(std::memory_order_acquire);
        if (producer_write_position_ - consumer_position >= capacity_) return nullptr;

        producer_has_reservation_ = true;
        return &storage_[producer_write_position_ % capacity_];
    }

    // Publish the producer's previously reserved and fully written slot. Release
    // ordering guarantees that the consumer sees all fields of T after it sees the
    // advanced write position.
    void publish_push() {
        require_producer_reservation("publish SPSC slot");
        producer_has_reservation_ = false;
        ++producer_write_position_;
        published_write_position_.store(producer_write_position_, std::memory_order_release);
    }

    // Abandon a reserved producer slot before publishing it. Its partially written
    // contents remain private and will be overwritten the next time that slot is
    // reserved; the consumer can never observe them.
    void cancel_push() {
        require_producer_reservation("cancel SPSC slot");
        producer_has_reservation_ = false;
    }

    // Return the next published value for read-only consumer access. A null pointer
    // means the ring is empty. The consumer must call finish_pop() after it has
    // finished reading the returned value and before it requests another one.
    [[nodiscard]] const T* try_begin_pop() {
        if (consumer_has_value_) {
            throw std::logic_error("SPSC consumer already has an active slot");
        }
        const std::uint64_t producer_position = published_write_position_.load(std::memory_order_acquire);
        if (consumer_read_position_ == producer_position) return nullptr;

        consumer_has_value_ = true;
        return &storage_[consumer_read_position_ % capacity_];
    }

    // Release the consumer's active slot. Release ordering guarantees that the
    // producer will not overwrite this slot until the consumer has finished every
    // read from it.
    void finish_pop() {
        if (!consumer_has_value_) {
            throw std::logic_error("cannot finish SPSC pop without an active slot");
        }
        consumer_has_value_ = false;
        ++consumer_read_position_;
        released_read_position_.store(consumer_read_position_, std::memory_order_release);
    }

    // These status queries are snapshots for reporting and tests. They are not a
    // replacement for try_reserve_push() or try_begin_pop(), because the other
    // thread can change the ring immediately after a query returns.
    [[nodiscard]] bool empty() const noexcept {
        return released_read_position_.load(std::memory_order_acquire) ==
               published_write_position_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        const std::uint64_t producer_position = published_write_position_.load(std::memory_order_acquire);
        const std::uint64_t consumer_position = released_read_position_.load(std::memory_order_acquire);
        return producer_position - consumer_position >= capacity_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::uint64_t producer_position = published_write_position_.load(std::memory_order_acquire);
        const std::uint64_t consumer_position = released_read_position_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(producer_position - consumer_position);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    [[nodiscard]] static std::size_t require_positive_capacity(const std::size_t capacity) {
        if (capacity == 0U) throw std::invalid_argument("SPSC ring capacity must be positive");
        return capacity;
    }

    void require_producer_reservation(const char* const action) const {
        if (!producer_has_reservation_) throw std::logic_error(std::string("cannot ") + action + " without a reserved slot");
    }

    const std::size_t capacity_;
    std::unique_ptr<T[]> storage_;

    // These are private to their owning side: the producer alone updates
    // producer_write_position_, and the consumer alone updates
    // consumer_read_position_. The matching atomic publication values are the only
    // positions shared between threads.
    std::uint64_t producer_write_position_{};
    std::uint64_t consumer_read_position_{};
    bool producer_has_reservation_{false};
    bool consumer_has_value_{false};
    alignas(64) std::atomic<std::uint64_t> published_write_position_{};
    alignas(64) std::atomic<std::uint64_t> released_read_position_{};
};

}  // namespace market_engine::app
