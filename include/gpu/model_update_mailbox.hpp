// This file declares the single-slot hand-off used for GPU-to-RTL model
// updates. Only the newest complete model matters, so a newer valid update may
// replace an unread older one. A mutex protects the one optional update while
// the GPU/application side publishes and the RTL worker side takes it.
#pragma once

#include <mutex>
#include <optional>

#include "gpu/gpu_protocol.hpp"

namespace market_engine::gpu {

class ModelUpdateMailbox {
public:
    // Store a complete valid update. The update must have a positive version
    // and valid thresholds, and must be newer than an unread mailbox value.
    void publish(ModelUpdate update);

    // Remove and return the newest waiting update, if one exists. The caller
    // owns the returned value and may apply it to RTL after releasing the lock.
    [[nodiscard]] std::optional<ModelUpdate> take();

    [[nodiscard]] bool has_update() const;

    // Say that the GPU side will publish no more updates for this live run.
    // Already published data remains available through take(). Closing lets the
    // RTL worker apply the final update and then exit cleanly.
    void close();
    [[nodiscard]] bool closed() const;

private:
    mutable std::mutex mutex_;
    std::optional<ModelUpdate> latest_update_;
    bool closed_{false};
};

}  // namespace market_engine::gpu
