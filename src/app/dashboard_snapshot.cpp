#include "app/dashboard_snapshot.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace market_engine::app {
void DashboardSnapshotStore::publish(DashboardSnapshot snapshot) {
    std::lock_guard lock(mutex_);
    latest_ = std::make_shared<const DashboardSnapshot>(std::move(snapshot));
}
std::shared_ptr<const DashboardSnapshot> DashboardSnapshotStore::latest() const {
    std::lock_guard lock(mutex_);
    return latest_;
}
std::string dashboard_snapshot_json(const DashboardSnapshot& snapshot) {
    nlohmann::json json{{"schemaVersion", snapshot.schema_version}, {"sequence", snapshot.sequence},
        {"publishedAtUnixMs", snapshot.published_at_unix_ms}, {"connectionState", snapshot.connection_state},
        {"state", snapshot.state}, {"inputFile", snapshot.input_file}, {"totalEvents", snapshot.total_events},
        {"processedEvents", snapshot.processed_events}, {"errorEvents", snapshot.error_events},
        {"eventsPerSecond", snapshot.events_per_second}, {"queue", {{"occupancy", snapshot.queue_occupancy}, {"capacity", snapshot.queue_capacity}}},
        {"featuresQ16", snapshot.features.values}, {"featuresValid", snapshot.features.valid},
        {"signal", {{"scoreQ16", snapshot.signal.score}, {"action", market::to_string(snapshot.signal.action)}, {"version", snapshot.signal.model_version}}},
        {"model", {{"weightsQ16", snapshot.model.weights}, {"buyThresholdQ16", snapshot.model.buy_threshold}, {"sellThresholdQ16", snapshot.model.sell_threshold}, {"version", snapshot.model.model_version}}},
        {"gpu", {{"batches", snapshot.gpu_batches}, {"updates", snapshot.gpu_updates}, {"squaredErrorQ16", snapshot.squared_error_sum_q16}, {"correct", snapshot.correct_predictions}, {"rows", snapshot.training_rows}, {"loss", snapshot.training_rows == 0U ? 0.0 : static_cast<double>(snapshot.squared_error_sum_q16) / static_cast<double>(snapshot.training_rows)}, {"accuracy", snapshot.training_rows == 0U ? 0.0 : static_cast<double>(snapshot.correct_predictions) / static_cast<double>(snapshot.training_rows)}, {"kernelMs", snapshot.gpu_kernel_ms}, {"trainingMs", snapshot.gpu_training_ms}, {"updateLatencyMs", snapshot.gpu_update_latency_ms}, {"latestUpdateUnixMs", snapshot.latest_update_unix_ms}}},
        {"performance", {{"rtlCyclesPerEvent", snapshot.rtl_cycles_per_event}, {"gpuTrainingMs", snapshot.gpu_training_ms}, {"updateLatencyMs", snapshot.gpu_update_latency_ms}}},
        {"dashboardClients", snapshot.dashboard_clients}};
    const auto levels = [](const auto& levels) { nlohmann::json result = nlohmann::json::array(); for (const auto& level : levels) result.push_back({{"priceTicks", level.price_ticks}, {"quantity", level.quantity}}); return result; };
    json["book"] = {{"bids", levels(snapshot.book.bids)}, {"asks", levels(snapshot.book.asks)}};
    const auto used = [](const auto& side) {
        return std::count_if(side.begin(), side.end(), [](const auto& level) { return level.quantity != 0U; });
    };
    json["bookDepth"] = {{"capacity", market::kBookDepth}, {"bidLevels", used(snapshot.book.bids)},
                         {"askLevels", used(snapshot.book.asks)}};
    json["recentEvents"] = nlohmann::json::array();
    for (const auto& event : snapshot.recent_events) {
        json["recentEvents"].push_back({{"timestampNs", event.timestamp_ns}, {"type", market::to_string(event.type)},
            {"side", market::to_string(event.side)}, {"priceTicks", event.price_ticks}, {"quantity", event.quantity}});
    }
    return json.dump();
}
}  // namespace market_engine::app
