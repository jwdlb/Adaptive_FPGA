// Integration tests for the dedicated RTL worker. The worker must be the only
// Verilator owner while a separate consumer reads the host SPSC result ring.
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "app/spsc_ring_buffer.hpp"
#include "gpu/model_update_mailbox.hpp"
#include "market/fixed_point.hpp"
#include "verilator/verilator_worker.hpp"

TEST_CASE("dedicated RTL worker streams directly into SPSC slots and applies mailbox updates",
          "[verilator][streaming]") {
#if MARKET_ENGINE_VERILATOR_AVAILABLE
    using namespace market_engine;

    const std::vector<market::MarketEvent> events{
        {.timestamp_ns = 100U, .type = market::EventType::Add, .side = market::Side::Bid,
         .price_ticks = 100, .quantity = 10U},
        {.timestamp_ns = 200U, .type = market::EventType::Add, .side = market::Side::Ask,
         .price_ticks = 102, .quantity = 10U},
    };

    market::ModelParameters initial{};
    initial.buy_threshold = market::fixed_point::kOne;
    initial.sell_threshold = -market::fixed_point::kOne;
    initial.model_version = 1U;

    gpu::ModelUpdateMailbox mailbox;
    gpu::ModelUpdate update{};
    update.update_version = 2U;
    update.weights[7] = market::fixed_point::kOne;
    update.buy_threshold = 0;
    update.sell_threshold = -market::fixed_point::kOne;
    mailbox.publish(update);
    // This test has no GPU thread that will close the mailbox later. Closing it
    // now tells the worker this is the complete finite update stream.
    mailbox.close();

    // Capacity one deliberately forces the producer and consumer to coordinate:
    // the worker cannot publish the second result until this test releases the
    // first result's one SPSC slot.
    app::SpscRingBuffer<verilator::RtlStreamResult> result_ring(1U);
    verilator::VerilatorWorker worker(events, 10U, initial, result_ring, mailbox,
                                      std::chrono::seconds(2));

    std::exception_ptr worker_error;
    std::atomic_bool worker_finished{false};
    std::thread rtl_thread([&] {
        try {
            worker.run();
        } catch (...) {
            worker_error = std::current_exception();
        }
        worker_finished.store(true, std::memory_order_release);
    });

    std::vector<verilator::RtlStreamResult> results;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (results.size() < events.size() && std::chrono::steady_clock::now() < deadline) {
        if (const verilator::RtlStreamResult* result = result_ring.try_begin_pop(); result != nullptr) {
            results.push_back(*result);
            result_ring.finish_pop();
        } else {
            std::this_thread::yield();
        }
    }
    rtl_thread.join();

    if (worker_error) std::rethrow_exception(worker_error);
    REQUIRE(worker_finished.load(std::memory_order_acquire));
    REQUIRE(results.size() == events.size());
    REQUIRE(results[0].event_index == 0U);
    REQUIRE(results[0].timestamp_ns == 100U);
    REQUIRE(results[0].error == market::BookError::None);
    REQUIRE_FALSE(results[0].features.valid);
    REQUIRE(results[1].event_index == 1U);
    REQUIRE(results[1].timestamp_ns == 200U);
    REQUIRE(results[1].error == market::BookError::None);
    REQUIRE(results[1].features.valid);

    const verilator::VerilatorWorkerMetrics metrics = worker.metrics();
    REQUIRE(metrics.input_events_accepted == events.size());
    REQUIRE(metrics.stream_results_published == events.size());
    REQUIRE(metrics.model_updates_applied == 1U);
    REQUIRE(metrics.rtl_cycles > 0U);
    REQUIRE(worker.active_parameters().model_version == 2U);
    REQUIRE(worker.active_parameters().update_count == 2U);
    REQUIRE_FALSE(mailbox.has_update());
#else
    SKIP("Verilator is unavailable in this build");
#endif
}
