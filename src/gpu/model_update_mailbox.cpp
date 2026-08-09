#include "gpu/model_update_mailbox.hpp"

#include <stdexcept>
#include <utility>

namespace market_engine::gpu {

void ModelUpdateMailbox::publish(ModelUpdate update) {
    // Version zero is never a valid active-model replacement. Reusing the
    // existing protocol check also keeps BUY strictly above SELL.
    validate_model_update(update, 0U);

    std::scoped_lock lock(mutex_);
    if (closed_) throw std::logic_error("cannot publish GPU model update after mailbox closure");
    if (latest_update_ && update.update_version <= latest_update_->update_version) {
        throw std::invalid_argument("GPU model update must be newer than the unread mailbox update");
    }
    latest_update_ = std::move(update);
}

std::optional<ModelUpdate> ModelUpdateMailbox::take() {
    std::scoped_lock lock(mutex_);
    if (!latest_update_) return std::nullopt;
    std::optional<ModelUpdate> result = std::move(latest_update_);
    latest_update_.reset();
    return result;
}

bool ModelUpdateMailbox::has_update() const {
    std::scoped_lock lock(mutex_);
    return latest_update_.has_value();
}

void ModelUpdateMailbox::close() {
    std::scoped_lock lock(mutex_);
    closed_ = true;
}

bool ModelUpdateMailbox::closed() const {
    std::scoped_lock lock(mutex_);
    return closed_;
}

}  // namespace market_engine::gpu
