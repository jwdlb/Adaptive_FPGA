#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "market/order_book.hpp"
#include "market/event.hpp"

namespace market_engine::app {

// Copy-only dashboard data. It deliberately contains no worker, ring, mailbox,
// OpenCL, or Verilator handles, so web clients can never affect the live path.
struct DashboardSnapshot {
    std::uint32_t schema_version{1};
    std::uint64_t sequence{};
    std::uint64_t published_at_unix_ms{};
    std::string connection_state{"disconnected"};
    std::string state{"idle"};
    std::string input_file{};
    std::uint64_t total_events{};
    std::uint64_t processed_events{};
    std::uint64_t error_events{};
    double events_per_second{};
    market::BookSnapshot book{};
    market::FeatureVector features{};
    market::Signal signal{};
    market::ModelParameters model{};
    std::size_t queue_occupancy{};
    std::size_t queue_capacity{};
    std::size_t gpu_batches{};
    std::size_t gpu_updates{};
    std::int64_t squared_error_sum_q16{};
    std::uint64_t correct_predictions{};
    std::uint64_t training_rows{};
    double gpu_kernel_ms{};
    double gpu_training_ms{};
    double gpu_update_latency_ms{};
    double rtl_cycles_per_event{};
    std::uint64_t latest_update_unix_ms{};
    std::size_t dashboard_clients{};
    std::vector<market::MarketEvent> recent_events{};
};

class DashboardSnapshotStore {
public:
    void publish(DashboardSnapshot snapshot);
    [[nodiscard]] std::shared_ptr<const DashboardSnapshot> latest() const;
private:
    mutable std::mutex mutex_;
    std::shared_ptr<const DashboardSnapshot> latest_{std::make_shared<DashboardSnapshot>()};
};

[[nodiscard]] std::string dashboard_snapshot_json(const DashboardSnapshot& snapshot);
}  // namespace market_engine::app
