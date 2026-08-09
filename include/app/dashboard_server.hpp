#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "app/dashboard_snapshot.hpp"

namespace market_engine::app {

struct DashboardServerOptions {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{8080};
    std::uint32_t update_hz{10};
};

// Beast/Asio server whose IO context lives exclusively on one background
// thread. WebSocket sessions retain at most one in-flight message and always
// fetch the newest snapshot for the next write.
class DashboardServer {
public:
    using ControlHandler = std::function<std::string(std::string_view)>;

    DashboardServer(DashboardServerOptions options,
                    std::shared_ptr<DashboardSnapshotStore> snapshots,
                    ControlHandler control_handler = {});
    ~DashboardServer();
    DashboardServer(const DashboardServer&) = delete;
    DashboardServer& operator=(const DashboardServer&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] std::size_t client_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string dashboard_html();

}  // namespace market_engine::app
