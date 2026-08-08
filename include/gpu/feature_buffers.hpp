#pragma once  // Prevent duplicate inclusion.
// 1. C++ fills A
//        A = Filling

// 2. C++ finishes A
//        A = Ready

// 3. begin_gpu_work(A)
//        A = InFlight

// 4. GpuModel reads gpu_batch(A) and polls completion
//        gpu_batch(A) confirms A is still InFlight

// 5. GPU copy finishes
//        A = Free
#include <array>
#include <cstddef>
#include <optional>

#include "gpu/gpu_protocol.hpp"

namespace market_engine::gpu {

// Each of the two host-side input slots is owned by exactly one stage. These
// names prevent the replay loop from overwriting a batch the GPU still uses.
enum class InputBufferState {
    Free,
    Filling,
    Ready,
    InFlight,
};

[[nodiscard]] const char* to_string(InputBufferState state) noexcept;

// Two alternating host-side input slots. GpuModel owns the matching two OpenCL
// device buffers and uses these state rules while copying batches to the GPU.
class FeatureBufferPool {
public:
    static constexpr std::size_t kBufferCount{2};

    // Find a Free slot and move it to Filling, if one is available.
    [[nodiscard]] std::optional<std::size_t> acquire_for_filling();
    // Move one known Free slot to Filling. Illegal state changes throw.
    void begin_filling(std::size_t index);
    // Store a finished 32 x 8 batch and move the slot from Filling to Ready.
    void finish_filling(std::size_t index, FeatureBatch batch);

    // Find a Ready slot and move it to InFlight. Only one simulated GPU job is
    // allowed at a time in this simple first version.
    [[nodiscard]] std::optional<std::size_t> acquire_for_gpu();
    // This changes one buffer from: Ready to InFlight. Illegal state changes throw.
    void begin_gpu_work(std::size_t index);
    // This lets GpuModel read a batch only after it has become InFlight. It is
    // used both as the OpenCL upload source and while checking completion.
    [[nodiscard]] const FeatureBatch& gpu_batch(std::size_t index) const;
    // Undo InFlight only when OpenCL rejected the upload before accepting any
    // command. This is error recovery, not a normal ownership transition.
    void cancel_gpu_work(std::size_t index);
    // GPU completion makes its slot safe to overwrite again.
    void finish_gpu_work(std::size_t index);

    // Return the current owner/state of buffer A (0) or B (1). [[nodiscard]]
    // reminds callers not to ignore this safety information.
    [[nodiscard]] InputBufferState state(std::size_t index) const;
    // Return true when one buffer is InFlight, meaning the GPU is still using it
    // and the replay side must not start another GPU job yet. noexcept means this
    // simple status check is guaranteed not to throw an error.
    [[nodiscard]] bool gpu_is_busy() const noexcept;

private:
    // One paired host-side slot. `batch` holds the 32 x 8 feature values while
    // C++ fills them or while OpenCL copies them to the matching GPU buffer;
    // `state` records who is currently allowed to use those values.
    struct Slot {
        FeatureBatch batch{};
        InputBufferState state{InputBufferState::Free};
    };

    // Check that a slot is in the state a requested action requires. For example,
    // beginning GPU work requires Ready; any other state is a hard logic error.
    void require_state(std::size_t index, InputBufferState expected, const char* action) const;
    // The two actual A/B slots. std::array fixes their number at kBufferCount,
    // so this object can never accidentally grow a third unsynchronised buffer.
    std::array<Slot, kBufferCount> slots_{};
};

}  // namespace market_engine::gpu
