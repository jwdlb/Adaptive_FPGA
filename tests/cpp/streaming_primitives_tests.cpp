// Tests for the small host-side building blocks needed before the RTL is moved
// to its own continuous execution thread.
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "app/spsc_ring_buffer.hpp"
#include "gpu/model_update_mailbox.hpp"
#include "verilator/rtl_stream.hpp"

namespace {

using market_engine::gpu::ModelUpdate;
using market_engine::gpu::ModelUpdateMailbox;

[[nodiscard]] ModelUpdate update(const std::uint64_t version) {
    ModelUpdate result{};
    result.update_version = version;
    result.buy_threshold = 20;
    result.sell_threshold = -20;
    return result;
}

}  // namespace

TEST_CASE("SPSC ring reserves, publishes, cancels, wraps, and preserves capacity", "[streaming][spsc]") {
    using market_engine::app::SpscRingBuffer;

    REQUIRE_THROWS_AS(SpscRingBuffer<int>(0U), std::invalid_argument);

    SpscRingBuffer<int> ring(2U);
    REQUIRE(ring.empty());
    REQUIRE_FALSE(ring.full());
    REQUIRE(ring.size() == 0U);

    int* first = ring.try_reserve_push();
    REQUIRE(first != nullptr);
    *first = 10;
    ring.publish_push();

    int* second = ring.try_reserve_push();
    REQUIRE(second != nullptr);
    *second = 20;
    ring.publish_push();
    REQUIRE(ring.full());
    REQUIRE(ring.try_reserve_push() == nullptr);

    const int* read_first = ring.try_begin_pop();
    REQUIRE(read_first != nullptr);
    REQUIRE(*read_first == 10);
    ring.finish_pop();

    int* wrapped = ring.try_reserve_push();
    REQUIRE(wrapped != nullptr);
    *wrapped = 30;
    ring.publish_push();

    const int* read_second = ring.try_begin_pop();
    REQUIRE(read_second != nullptr);
    REQUIRE(*read_second == 20);
    ring.finish_pop();
    const int* read_wrapped = ring.try_begin_pop();
    REQUIRE(read_wrapped != nullptr);
    REQUIRE(*read_wrapped == 30);
    ring.finish_pop();
    REQUIRE(ring.empty());

    int* cancelled = ring.try_reserve_push();
    REQUIRE(cancelled != nullptr);
    *cancelled = 99;
    ring.cancel_push();
    REQUIRE(ring.empty());

    int* replacement = ring.try_reserve_push();
    REQUIRE(replacement != nullptr);
    *replacement = 40;
    ring.publish_push();
    const int* read_replacement = ring.try_begin_pop();
    REQUIRE(read_replacement != nullptr);
    REQUIRE(*read_replacement == 40);
    ring.finish_pop();
}

TEST_CASE("SPSC ring rejects illegal producer and consumer ownership jumps", "[streaming][spsc]") {
    market_engine::app::SpscRingBuffer<int> ring(1U);

    int* slot = ring.try_reserve_push();
    REQUIRE(slot != nullptr);
    REQUIRE_THROWS_AS(ring.try_reserve_push(), std::logic_error);
    REQUIRE_THROWS_AS(ring.finish_pop(), std::logic_error);
    *slot = 7;
    ring.publish_push();
    REQUIRE_THROWS_AS(ring.publish_push(), std::logic_error);

    const int* value = ring.try_begin_pop();
    REQUIRE(value != nullptr);
    REQUIRE_THROWS_AS(ring.try_begin_pop(), std::logic_error);
    ring.finish_pop();
}

TEST_CASE("SPSC ring transfers values safely between one producer and one consumer", "[streaming][spsc]") {
    constexpr std::uint32_t count{10000U};
    market_engine::app::SpscRingBuffer<std::uint32_t> ring(17U);

    std::thread producer([&] {
        for (std::uint32_t value = 0U; value < count;) {
            if (std::uint32_t* slot = ring.try_reserve_push(); slot != nullptr) {
                *slot = value;
                ring.publish_push();
                ++value;
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (std::uint32_t expected = 0U; expected < count;) {
        if (const std::uint32_t* value = ring.try_begin_pop(); value != nullptr) {
            REQUIRE(*value == expected);
            ring.finish_pop();
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    REQUIRE(ring.empty());
}

TEST_CASE("model update mailbox keeps only the newest complete update", "[streaming][mailbox]") {
    ModelUpdateMailbox mailbox;
    REQUIRE_FALSE(mailbox.has_update());

    mailbox.publish(update(2U));
    REQUIRE(mailbox.has_update());
    mailbox.publish(update(3U));
    REQUIRE(mailbox.has_update());

    const auto newest = mailbox.take();
    REQUIRE(newest.has_value());
    REQUIRE(newest->update_version == 3U);
    REQUIRE_FALSE(mailbox.has_update());
    REQUIRE_FALSE(mailbox.take().has_value());

    mailbox.publish(update(4U));
    REQUIRE_THROWS_AS(mailbox.publish(update(4U)), std::invalid_argument);
    REQUIRE_THROWS_AS(mailbox.publish(update(3U)), std::invalid_argument);

    ModelUpdate invalid = update(5U);
    invalid.buy_threshold = invalid.sell_threshold;
    REQUIRE_THROWS_AS(mailbox.publish(invalid), std::invalid_argument);
}

TEST_CASE("RTL stream result keeps compact event control and feature data", "[streaming][rtl]") {
    market_engine::verilator::RtlStreamResult result{};
    result.event_index = 42U;
    result.timestamp_ns = 420U;
    result.features.valid = true;
    result.features.values[0] = 123;

    REQUIRE(result.event_index == 42U);
    REQUIRE(result.timestamp_ns == 420U);
    REQUIRE(result.error == market_engine::market::BookError::None);
    REQUIRE(result.features.valid);
    REQUIRE(result.features.values[0] == 123);
}
