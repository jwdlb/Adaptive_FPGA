// This file manages the two alternating feature-buffer slots used between
// replay and GPU work. It stores each completed 32 x 8 batch in a host-side
// slot and controls the ownership states:
//
//   Free -> Filling -> Ready -> InFlight -> Free
//
// It does not allocate GPU memory or perform OpenCL transfers. Those operations
// are handled by gpu_model.cpp; this file only makes sure the CPU and GPU never
// use the same slot at the same time.
#include "gpu/feature_buffers.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace market_engine::gpu {

// Convert the internal state enum into readable text for diagnostics. This is
// used when an illegal transition is reported, for example when code tries to
// fill a buffer that is still InFlight on the GPU.
const char* to_string(const InputBufferState state) noexcept {
    switch (state) {
        case InputBufferState::Free: return "Free";
        case InputBufferState::Filling: return "Filling";
        case InputBufferState::Ready: return "Ready";
        case InputBufferState::InFlight: return "InFlight";
    }
    return "unknown";
}

// Find the first slot that is genuinely free for the replay side to use. The
// successful path changes Free -> Filling before returning the slot number, so
// another caller cannot accidentally claim the same slot. If both A and B are
// busy, return no index; the caller should continue processing/polling rather
// than overwrite either buffer.
std::optional<std::size_t> FeatureBufferPool::acquire_for_filling() {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].state == InputBufferState::Free) {
            begin_filling(index);
            return index;
        }
    }
    return std::nullopt;
}

// Begin writing a new feature batch into one specific slot. Only an unused
// Free slot may enter Filling. The state check is important: it prevents an
// InFlight GPU buffer from being reused by the CPU.
void FeatureBufferPool::begin_filling(const std::size_t index) {
    require_state(index, InputBufferState::Free, "begin filling");
    slots_[index].state = InputBufferState::Filling;
}

// Finish writing a batch into a slot. Moving the batch into the slot keeps the
// exact 32-by-8 values owned by the pool, then Ready tells the GPU side that it
// may read them. The slot must have been Filling first; callers cannot publish
// half-written data or replace a batch already waiting for the GPU.
void FeatureBufferPool::finish_filling(const std::size_t index, FeatureBatch batch) {
    require_state(index, InputBufferState::Filling, "finish filling");
    slots_[index].batch = std::move(batch);
    slots_[index].state = InputBufferState::Ready;
}

// Find a completed Ready batch for the GPU. This simple version permits only
// one GPU operation at a time, so it returns no slot while another slot is
// InFlight. Once a slot is found, begin_gpu_work changes Ready -> InFlight.
std::optional<std::size_t> FeatureBufferPool::acquire_for_gpu() {
    if (gpu_is_busy()) return std::nullopt;
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].state == InputBufferState::Ready) {
            begin_gpu_work(index);
            return index;
        }
    }
    return std::nullopt;
}

// Mark one Ready slot as owned by the GPU. This is the protection point between
// producing a batch and starting its device upload: after this call, the CPU
// must leave the slot's values unchanged until finish_gpu_work().
void FeatureBufferPool::begin_gpu_work(const std::size_t index) {
    require_state(index, InputBufferState::Ready, "begin GPU work");
    if (gpu_is_busy()) {
        throw std::logic_error("cannot begin GPU work while another input buffer is InFlight");
    }
    slots_[index].state = InputBufferState::InFlight;
}

// Give completion/polling code read-only access to the batch currently owned by
// the GPU. The upload code also uses this reference as its CPU-side source
// pointer. It remains stable while the state is InFlight and must not be
// modified or replaced until finish_gpu_work() is called.
const FeatureBatch& FeatureBufferPool::gpu_batch(const std::size_t index) const {
    require_state(index, InputBufferState::InFlight, "read GPU batch");
    return slots_[index].batch;
}

// Return a slot to Ready after an upload submission was rejected before OpenCL
// accepted it. The batch is still complete and can safely be tried again. This
// is deliberately separate from finish_gpu_work(), which is used only after a
// real InFlight GPU operation completes.
void FeatureBufferPool::cancel_gpu_work(const std::size_t index) {
    require_state(index, InputBufferState::InFlight, "cancel GPU work");
    slots_[index].state = InputBufferState::Ready;
}

// Mark a finished GPU operation as safe to overwrite. This is the final state
// transition InFlight -> Free, normally called only after its OpenCL event says
// the device has finished reading the slot.
void FeatureBufferPool::finish_gpu_work(const std::size_t index) {
    require_state(index, InputBufferState::InFlight, "finish GPU work");
    slots_[index].state = InputBufferState::Free;
}

// Return the current state of slot A (index 0) or slot B (index 1). An invalid
// index is a programming error, so throw instead of silently inspecting memory
// outside the two-slot pool.
InputBufferState FeatureBufferPool::state(const std::size_t index) const {
    if (index >= slots_.size()) throw std::out_of_range("input buffer index is outside the two-buffer pool");
    return slots_[index].state;
}

// Check the state required by a requested operation and produce one consistent
// error message when it is not met. All public transition/read functions use
// this helper, which keeps illegal jumps such as InFlight -> Filling rejected.
void FeatureBufferPool::require_state(const std::size_t index, const InputBufferState expected, const char* action) const {
    const InputBufferState actual = state(index);
    if (actual != expected) {
        throw std::logic_error(std::string("cannot ") + action + " for input buffer " + std::to_string(index) +
                               ": expected " + to_string(expected) + ", found " + to_string(actual));
    }
}

// Report whether either input slot is currently owned by the GPU. The scan is
// tiny (there are exactly two slots), and noexcept makes it safe to use as a
// guard before starting another upload.
bool FeatureBufferPool::gpu_is_busy() const noexcept {
    for (const Slot& slot : slots_) {
        if (slot.state == InputBufferState::InFlight) return true;
    }
    return false;
}

}  // namespace market_engine::gpu
