// Reusable OpenCL connection and GPU smoke test for Adaptive_FPGA.
//
// This file selects a GPU, creates the OpenCL resources needed to use it, and
// compiles a tiny kernel that doubles float values. The doubling operation is a
// health check for the complete host-to-GPU path: device selection, context and
// queue creation, kernel compilation, buffer transfers, kernel execution, and
// result retrieval. It is infrastructure for future GPU model training, not
// the trading strategy or FPGA RTL implementation itself.
#include "gpu/gpu_model.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

#if MARKET_ENGINE_HAS_OPENCL
#include <CL/cl.h>
#endif

namespace market_engine::gpu {
namespace {

#if MARKET_ENGINE_HAS_OPENCL
// Read the OpenCL kernel source code from disk before compiling it for the GPU.
[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open OpenCL kernel: " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Return the compiler log produced while OpenCL builds a kernel program.
// This is included in errors so a broken .cl file is easier to diagnose.
[[nodiscard]] std::string program_build_log(cl_program program, cl_device_id device) {
    std::size_t size = 0;
    if (clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size) != CL_SUCCESS) {
        return "<OpenCL did not provide a build log>";
    }
    std::string log(size, '\0');
    if (clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, size, log.data(), nullptr) != CL_SUCCESS) {
        return "<OpenCL did not provide a readable build log>";
    }
    if (!log.empty() && log.back() == '\0') log.pop_back();
    return log;
}

// Re-discover the low-level OpenCL device ID described by a saved device record.
// OpenclDeviceInfo stores stable list positions, while OpenCL calls need IDs.
[[nodiscard]] cl_device_id find_device_id(const app::OpenclDeviceInfo& selected) {
    // Re-enumerate platforms and confirm the selected one still exists.
    cl_uint platform_count = 0;
    if (clGetPlatformIDs(0, nullptr, &platform_count) != CL_SUCCESS || selected.platform_index >= platform_count) {
        throw std::runtime_error("selected OpenCL platform is no longer available");
    }
    std::vector<cl_platform_id> platforms(platform_count);
    if (clGetPlatformIDs(platform_count, platforms.data(), nullptr) != CL_SUCCESS) {
        throw std::runtime_error("could not retrieve selected OpenCL platform");
    }
    const cl_platform_id platform = platforms[selected.platform_index];
    // Look up all device types so the saved device index matches discovery.
    cl_uint device_count = 0;
    if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &device_count) != CL_SUCCESS ||
        selected.device_index >= device_count) {
        throw std::runtime_error("selected OpenCL device is no longer available");
    }
    std::vector<cl_device_id> devices(device_count);
    if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, device_count, devices.data(), nullptr) != CL_SUCCESS) {
        throw std::runtime_error("could not retrieve selected OpenCL device");
    }
    return devices[selected.device_index];
}

// Convert every unsuccessful OpenCL status code into a descriptive C++ error.
void check_opencl(cl_int status, std::string_view action) {
    if (status != CL_SUCCESS) {
        throw std::runtime_error(std::string(action) + " failed (OpenCL error " + std::to_string(status) + ")");
    }
}
#endif

}  // namespace

class GpuModel::Impl {
public:
    // Human-readable record of the GPU selected for this model instance.
    app::OpenclDeviceInfo selected_device;

#if MARKET_ENGINE_HAS_OPENCL
    // OpenCL objects owned by this model. They are released in reverse order by cleanup().
    cl_context context{nullptr};
    cl_command_queue queue{nullptr};
    cl_program program{nullptr};
    cl_kernel smoke_kernel{nullptr};
    cl_kernel phase6_update_kernel{nullptr};
    cl_kernel training_kernel{nullptr};
    cl_kernel training_reduce_kernel{nullptr};
    cl_mem input_buffer{nullptr};
    cl_mem output_buffer{nullptr};
    cl_event input_complete{nullptr};
    cl_event kernel_complete{nullptr};
    cl_event output_complete{nullptr};
    std::size_t buffer_capacity{0};

    // Streaming-path OpenCL objects. The input buffer is allocated with host
    // mapping support so GpuWorker can write its `[N][8]` values directly into
    // OpenCL-visible memory without an intermediate host-side upload buffer.
    cl_mem stream_input_buffer{nullptr};
    cl_mem stream_label_buffer{nullptr};
    cl_mem stream_update_buffer{nullptr};
    cl_mem stream_gradient_buffer{nullptr};
    cl_mem stream_loss_buffer{nullptr};
    cl_mem stream_correct_buffer{nullptr};
    cl_mem stream_metrics_buffer{nullptr};
    cl_event stream_unmap_complete{nullptr};
    cl_event stream_kernel_complete{nullptr};
    cl_event stream_reduce_complete{nullptr};
    cl_event stream_read_complete{nullptr};
    std::int32_t* mapped_stream_values{nullptr};
    std::int32_t* mapped_stream_labels{nullptr};
    std::size_t mapped_stream_rows{0};
    std::size_t stream_capacity_rows{0};
    std::array<std::int32_t, market::FeatureVector::kFeatureCount + 2U> stream_update_values{};
    std::array<std::int32_t, market::FeatureVector::kFeatureCount + 2U> configured_training_model{};
    bool has_configured_training_model{false};
    std::array<std::int64_t, 3U> stream_training_metrics{};
    std::optional<std::uint64_t> pending_stream_update_version{};
    std::optional<std::chrono::steady_clock::time_point> training_started{};

    // Create every OpenCL resource needed by the reusable smoke-test operation.
    explicit Impl(app::OpenclDeviceInfo device) : selected_device(std::move(device)) {
        try {
            // Convert the selected device's displayed indices back into an OpenCL ID.
            const cl_device_id device_id = find_device_id(selected_device);
            cl_int status = CL_SUCCESS;
            // A context owns resources associated with one selected GPU.
            context = clCreateContext(nullptr, 1, &device_id, nullptr, nullptr, &status);
            check_opencl(status, "creating OpenCL context");

            // The command queue orders copies and kernel launches sent to this GPU.
            const cl_queue_properties queue_properties[] = {CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0};
            queue = clCreateCommandQueueWithProperties(context, device_id, queue_properties, &status);
            check_opencl(status, "creating OpenCL command queue");

            // Load and compile the small OpenCL kernel used to verify GPU execution.
            const std::filesystem::path kernel_path =
                std::filesystem::path(MARKET_ENGINE_OPENCL_KERNEL_DIR) / "smoke_test.cl";
            const std::string source = read_text_file(kernel_path);
            const char* source_pointer = source.c_str();
            const std::size_t source_size = source.size();
            program = clCreateProgramWithSource(context, 1, &source_pointer, &source_size, &status);
            check_opencl(status, "creating OpenCL program from " + kernel_path.string());
            status = clBuildProgram(program, 1, &device_id, nullptr, nullptr, nullptr);
            if (status != CL_SUCCESS) {
                throw std::runtime_error("building OpenCL kernel " + kernel_path.string() + " failed (OpenCL error " +
                                         std::to_string(status) + ")\nBuild log:\n" + program_build_log(program, device_id));
            }
            // Obtain the compiled function named `double_values` from the program.
            smoke_kernel = clCreateKernel(program, "double_values", &status);
            check_opencl(status, "creating double_values OpenCL kernel");
            phase6_update_kernel = clCreateKernel(program, "phase6_model_update", &status);
            check_opencl(status, "creating phase6_model_update OpenCL kernel");
            training_kernel = clCreateKernel(program, "regression_row_gradients", &status);
            check_opencl(status, "creating regression_row_gradients OpenCL kernel");
            training_reduce_kernel = clCreateKernel(program, "regression_apply_batch", &status);
            check_opencl(status, "creating regression_apply_batch OpenCL kernel");

        } catch (...) {
            // Constructor failures must not leak resources already created above.
            cleanup();
            throw;
        }
    }

    // Release all OpenCL resources when the model is destroyed.
    ~Impl() {
        cleanup();
    }

    // Release owned OpenCL objects and reset their handles to prevent reuse.
    void cleanup() noexcept {
        // A mapped OpenCL buffer must be unmapped before its queue/context is
        // released. Cleanup cannot report an error, so finish any best-effort
        // unmap here and then continue releasing every remaining resource.
        if (mapped_stream_values != nullptr && queue != nullptr && stream_input_buffer != nullptr) {
            cl_event unmap_event{nullptr};
            if (clEnqueueUnmapMemObject(queue, stream_input_buffer, mapped_stream_values, 0, nullptr,
                                        &unmap_event) == CL_SUCCESS) {
                clWaitForEvents(1, &unmap_event);
                clReleaseEvent(unmap_event);
            }
            mapped_stream_values = nullptr;
            mapped_stream_rows = 0;
        }
        if (mapped_stream_labels != nullptr && queue != nullptr && stream_label_buffer != nullptr) {
            cl_event unmap_event{nullptr};
            if (clEnqueueUnmapMemObject(queue, stream_label_buffer, mapped_stream_labels, 0, nullptr,
                                        &unmap_event) == CL_SUCCESS) {
                clWaitForEvents(1, &unmap_event);
                clReleaseEvent(unmap_event);
            }
            mapped_stream_labels = nullptr;
        }
        release_event(stream_read_complete);
        release_event(stream_reduce_complete);
        release_event(stream_kernel_complete);
        release_event(stream_unmap_complete);
        if (stream_update_buffer != nullptr) {
            clReleaseMemObject(stream_update_buffer);
            stream_update_buffer = nullptr;
        }
        for (cl_mem* buffer : {&stream_metrics_buffer, &stream_correct_buffer, &stream_loss_buffer, &stream_gradient_buffer}) {
            if (*buffer != nullptr) { clReleaseMemObject(*buffer); *buffer = nullptr; }
        }
        if (stream_input_buffer != nullptr) {
            clReleaseMemObject(stream_input_buffer);
            stream_input_buffer = nullptr;
        }
        if (stream_label_buffer != nullptr) {
            clReleaseMemObject(stream_label_buffer);
            stream_label_buffer = nullptr;
        }
        // Completion events refer to operations using the buffers, so release them first.
        release_event(output_complete);
        release_event(kernel_complete);
        release_event(input_complete);
        if (output_buffer != nullptr) {
            clReleaseMemObject(output_buffer);
            output_buffer = nullptr;
        }
        if (input_buffer != nullptr) {
            clReleaseMemObject(input_buffer);
            input_buffer = nullptr;
        }
        if (smoke_kernel != nullptr) {
            clReleaseKernel(smoke_kernel);
            smoke_kernel = nullptr;
        }
        if (phase6_update_kernel != nullptr) {
            clReleaseKernel(phase6_update_kernel);
            phase6_update_kernel = nullptr;
        }
        if (training_kernel != nullptr) {
            clReleaseKernel(training_kernel);
            training_kernel = nullptr;
        }
        if (training_reduce_kernel != nullptr) { clReleaseKernel(training_reduce_kernel); training_reduce_kernel = nullptr; }
        if (program != nullptr) {
            clReleaseProgram(program);
            program = nullptr;
        }
        if (queue != nullptr) {
            clReleaseCommandQueue(queue);
            queue = nullptr;
        }
        if (context != nullptr) {
            clReleaseContext(context);
            context = nullptr;
        }
        buffer_capacity = 0;
        stream_capacity_rows = 0;
        pending_stream_update_version.reset();
    }

    // Ensure the reusable device buffers can hold `count` float values.
    // Existing larger buffers are retained to avoid needless reallocations.
    void ensure_buffer_capacity(std::size_t count) {
        if (count <= buffer_capacity) return;
        // Reject a size that would overflow the byte-count calculation below.
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
            throw std::runtime_error("GPU smoke-test input is too large");
        }
        // The old buffers are too small, so replace both with a matching larger pair.
        if (output_buffer != nullptr) {
            clReleaseMemObject(output_buffer);
            output_buffer = nullptr;
        }
        if (input_buffer != nullptr) {
            clReleaseMemObject(input_buffer);
            input_buffer = nullptr;
        }
        cl_int status = CL_SUCCESS;
        const std::size_t bytes = count * sizeof(float);
        // Input travels host → GPU; output travels GPU → host.
        input_buffer = clCreateBuffer(context, CL_MEM_READ_ONLY, bytes, nullptr, &status);
        check_opencl(status, "creating OpenCL input buffer");
        output_buffer = clCreateBuffer(context, CL_MEM_WRITE_ONLY, bytes, nullptr, &status);
        check_opencl(status, "creating OpenCL output buffer");
        buffer_capacity = count;
    }

    // Send floats to the GPU, run double_values, and return the computed floats.
    [[nodiscard]] std::vector<float> double_values(std::span<const float> input) {
        if (input.empty()) return {};
        // The kernel's count argument is a 32-bit OpenCL unsigned integer.
        if (input.size() > std::numeric_limits<cl_uint>::max()) {
            throw std::runtime_error("GPU smoke-test input has too many values");
        }
        ensure_buffer_capacity(input.size());
        // These events belong to the previous invocation and are no longer needed.
        release_event(output_complete);
        release_event(kernel_complete);
        release_event(input_complete);

        const std::size_t bytes = input.size_bytes();
        // Enqueue a non-blocking host-to-device copy and retain its completion event.
        check_opencl(clEnqueueWriteBuffer(queue, input_buffer, CL_FALSE, 0, bytes, input.data(), 0, nullptr, &input_complete),
                     "sending smoke-test input to GPU");
        const cl_uint count = static_cast<cl_uint>(input.size());
        // Set the kernel parameters: input buffer, output buffer, and element count.
        check_opencl(clSetKernelArg(smoke_kernel, 0, sizeof(input_buffer), &input_buffer), "setting smoke kernel input");
        check_opencl(clSetKernelArg(smoke_kernel, 1, sizeof(output_buffer), &output_buffer), "setting smoke kernel output");
        check_opencl(clSetKernelArg(smoke_kernel, 2, sizeof(count), &count), "setting smoke kernel count");
        const std::size_t global_size = input.size();
        // Launch one kernel work-item per input value after the upload completes.
        check_opencl(clEnqueueNDRangeKernel(queue, smoke_kernel, 1, nullptr, &global_size, nullptr, 1, &input_complete,
                                            &kernel_complete),
                     "running smoke kernel");

        std::vector<float> output(input.size());
        // Read only after the kernel completes, then wait for this result to arrive.
        check_opencl(clEnqueueReadBuffer(queue, output_buffer, CL_FALSE, 0, bytes, output.data(), 1, &kernel_complete,
                                         &output_complete),
                     "receiving smoke-test output from GPU");
        check_opencl(clWaitForEvents(1, &output_complete), "waiting for smoke-test output");
        return output;
    }

    // Ensure the reusable mapped OpenCL input and small update-output buffer are
    // large enough for `rows` by eight Q16.16 feature values.
    void ensure_stream_capacity(const std::size_t rows) {
        if (rows == 0U) throw std::invalid_argument("GPU streaming batch must contain at least one row");
        if (rows <= stream_capacity_rows) return;
        if (mapped_stream_values != nullptr || mapped_stream_labels != nullptr || pending_stream_update_version) {
            throw std::logic_error("cannot resize a mapped or in-flight GPU streaming buffer");
        }
        if (rows > std::numeric_limits<std::size_t>::max() /
                       (market::FeatureVector::kFeatureCount * sizeof(std::int32_t))) {
            throw std::runtime_error("GPU streaming batch is too large");
        }
        if (stream_update_buffer != nullptr) {
            clReleaseMemObject(stream_update_buffer);
            stream_update_buffer = nullptr;
        }
        if (stream_input_buffer != nullptr) {
            clReleaseMemObject(stream_input_buffer);
            stream_input_buffer = nullptr;
        }
        if (stream_label_buffer != nullptr) {
            clReleaseMemObject(stream_label_buffer);
            stream_label_buffer = nullptr;
        }
        for (cl_mem* buffer : {&stream_metrics_buffer, &stream_correct_buffer, &stream_loss_buffer, &stream_gradient_buffer}) {
            if (*buffer != nullptr) { clReleaseMemObject(*buffer); *buffer = nullptr; }
        }

        const std::size_t input_bytes = rows * market::FeatureVector::kFeatureCount * sizeof(std::int32_t);
        cl_int status = CL_SUCCESS;
        stream_input_buffer = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR,
                                             input_bytes, nullptr, &status);
        check_opencl(status, "creating mapped GPU streaming input buffer");
        stream_label_buffer = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR,
                                             rows * sizeof(std::int32_t), nullptr, &status);
        check_opencl(status, "creating mapped GPU training-label buffer");
        stream_update_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                              stream_update_values.size() * sizeof(std::int32_t), nullptr, &status);
        check_opencl(status, "creating GPU streaming model-update buffer");
        stream_gradient_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE, input_bytes * sizeof(std::int64_t) / sizeof(std::int32_t), nullptr, &status);
        check_opencl(status, "creating GPU regression-gradient buffer");
        stream_loss_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE, rows * sizeof(std::int64_t), nullptr, &status);
        check_opencl(status, "creating GPU regression-loss buffer");
        stream_correct_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE, rows * sizeof(std::int32_t), nullptr, &status);
        check_opencl(status, "creating GPU regression-accuracy buffer");
        stream_metrics_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE, stream_training_metrics.size() * sizeof(std::int64_t), nullptr, &status);
        check_opencl(status, "creating GPU regression-metrics buffer");
        stream_update_values.fill(0);
        stream_update_values[market::FeatureVector::kFeatureCount] = 16384;
        stream_update_values[market::FeatureVector::kFeatureCount + 1U] = -16384;
        if (has_configured_training_model) stream_update_values = configured_training_model;
        check_opencl(clEnqueueWriteBuffer(queue, stream_update_buffer, CL_TRUE, 0,
                                          stream_update_values.size() * sizeof(std::int32_t),
                                          stream_update_values.data(), 0, nullptr, nullptr),
                     "initialising GPU training model");
        stream_capacity_rows = rows;
    }

    [[nodiscard]] std::span<std::int32_t> map_stream_feature_rows(const std::size_t rows) {
        if (mapped_stream_values != nullptr) {
            throw std::logic_error("GPU streaming input is already mapped for filling");
        }
        if (pending_stream_update_version) {
            throw std::logic_error("cannot map GPU streaming input while its prior update is in flight");
        }
        ensure_stream_capacity(rows);
        const std::size_t value_count = rows * market::FeatureVector::kFeatureCount;
        cl_int status = CL_SUCCESS;
        mapped_stream_values = static_cast<std::int32_t*>(clEnqueueMapBuffer(
            queue, stream_input_buffer, CL_TRUE, CL_MAP_WRITE, 0,
            value_count * sizeof(std::int32_t), 0, nullptr, nullptr, &status));
        check_opencl(status, "mapping GPU streaming input buffer");
        mapped_stream_rows = rows;
        return {mapped_stream_values, value_count};
    }

    void submit_phase6_model_update(const std::uint64_t version) {
        if (mapped_stream_values == nullptr || mapped_stream_rows == 0U) {
            throw std::logic_error("cannot submit an unmapped or empty GPU streaming batch");
        }
        if (version == 0U) throw std::invalid_argument("GPU model-update version must be positive");
        if (pending_stream_update_version) {
            throw std::logic_error("GPU streaming model update is already in flight");
        }
        const std::size_t feature_count = mapped_stream_rows * market::FeatureVector::kFeatureCount;
        if (feature_count > std::numeric_limits<cl_uint>::max()) {
            throw std::runtime_error("GPU streaming batch has too many feature values");
        }

        release_event(stream_read_complete);
        release_event(stream_kernel_complete);
        release_event(stream_unmap_complete);
        check_opencl(clEnqueueUnmapMemObject(queue, stream_input_buffer, mapped_stream_values,
                                             0, nullptr, &stream_unmap_complete),
                     "unmapping GPU streaming input buffer");
        mapped_stream_values = nullptr;
        mapped_stream_rows = 0U;

        const cl_uint opencl_feature_count = static_cast<cl_uint>(feature_count);
        check_opencl(clSetKernelArg(phase6_update_kernel, 0, sizeof(stream_input_buffer), &stream_input_buffer),
                     "setting Phase 6 kernel input");
        check_opencl(clSetKernelArg(phase6_update_kernel, 1, sizeof(stream_update_buffer), &stream_update_buffer),
                     "setting Phase 6 kernel output");
        check_opencl(clSetKernelArg(phase6_update_kernel, 2, sizeof(opencl_feature_count), &opencl_feature_count),
                     "setting Phase 6 kernel feature count");
        constexpr std::size_t global_size{1U};
        check_opencl(clEnqueueNDRangeKernel(queue, phase6_update_kernel, 1, nullptr, &global_size, nullptr,
                                            1, &stream_unmap_complete, &stream_kernel_complete),
                     "running Phase 6 GPU model-update kernel");
        check_opencl(clEnqueueReadBuffer(queue, stream_update_buffer, CL_FALSE, 0,
                                         stream_update_values.size() * sizeof(std::int32_t),
                                         stream_update_values.data(), 1, &stream_kernel_complete,
                                         &stream_read_complete),
                     "reading Phase 6 GPU model update");
        check_opencl(clFlush(queue), "starting Phase 6 GPU model update");
        pending_stream_update_version = version;
    }

    [[nodiscard]] std::optional<ModelUpdate> poll_phase6_model_update() {
        if (!pending_stream_update_version) return std::nullopt;

        cl_int execution_status = CL_QUEUED;
        check_opencl(clGetEventInfo(stream_read_complete, CL_EVENT_COMMAND_EXECUTION_STATUS,
                                    sizeof(execution_status), &execution_status, nullptr),
                     "polling Phase 6 GPU model update");
        if (execution_status < 0) {
            throw std::runtime_error("Phase 6 GPU model update failed (OpenCL error " +
                                     std::to_string(execution_status) + ")");
        }
        if (execution_status != CL_COMPLETE) return std::nullopt;

        ModelUpdate update{};
        update.update_version = *pending_stream_update_version;
        for (std::size_t index = 0; index < update.weights.size(); ++index) {
            update.weights[index] = stream_update_values[index];
        }
        update.buy_threshold = stream_update_values[update.weights.size()];
        update.sell_threshold = stream_update_values[update.weights.size() + 1U];
        release_event(stream_read_complete);
        release_event(stream_kernel_complete);
        release_event(stream_unmap_complete);
        pending_stream_update_version.reset();
        return update;
    }

    void discard_stream_feature_rows() {
        if (mapped_stream_values == nullptr) return;
        cl_event unmap_complete{nullptr};
        check_opencl(clEnqueueUnmapMemObject(queue, stream_input_buffer, mapped_stream_values,
                                             0, nullptr, &unmap_complete),
                     "discarding mapped GPU streaming input buffer");
        mapped_stream_values = nullptr;
        mapped_stream_rows = 0U;
        check_opencl(clWaitForEvents(1, &unmap_complete), "waiting to discard GPU streaming input buffer");
        release_event(unmap_complete);
    }

    [[nodiscard]] MappedTrainingBatch map_training_batch(const std::size_t rows) {
        const std::span<std::int32_t> features = map_stream_feature_rows(rows);
        cl_int status = CL_SUCCESS;
        mapped_stream_labels = static_cast<std::int32_t*>(clEnqueueMapBuffer(
            queue, stream_label_buffer, CL_TRUE, CL_MAP_WRITE, 0, rows * sizeof(std::int32_t),
            0, nullptr, nullptr, &status));
        check_opencl(status, "mapping GPU training-label buffer");
        return {.features = features, .labels = {mapped_stream_labels, rows}};
    }

    void set_training_model(const market::ModelParameters& model) {
        if (mapped_stream_values != nullptr || mapped_stream_labels != nullptr || pending_stream_update_version)
            throw std::logic_error("cannot replace GPU training model while a batch is active");
        if (model.model_version == 0U || model.buy_threshold <= model.sell_threshold)
            throw std::invalid_argument("invalid initial GPU training model");
        configured_training_model.fill(0);
        for (std::size_t index = 0; index < model.weights.size(); ++index) configured_training_model[index] = model.weights[index];
        configured_training_model[model.weights.size()] = model.buy_threshold;
        configured_training_model[model.weights.size() + 1U] = model.sell_threshold;
        has_configured_training_model = true;
    }

    void submit_training_batch(const std::uint64_t version, const std::int32_t learning_rate_q16, const std::int32_t l2_q16) {
        if (mapped_stream_values == nullptr || mapped_stream_labels == nullptr || mapped_stream_rows == 0U) {
            throw std::logic_error("cannot train without mapped features and labels");
        }
        if (version == 0U || learning_rate_q16 <= 0 || l2_q16 < 0) {
            throw std::invalid_argument("GPU training version, learning rate, and L2 regularisation are invalid");
        }
        if (pending_stream_update_version) throw std::logic_error("GPU training update is already in flight");

        const cl_uint rows_to_train = static_cast<cl_uint>(mapped_stream_rows);
        const std::size_t row_global_size = mapped_stream_rows;
        release_event(stream_read_complete); release_event(stream_kernel_complete); release_event(stream_reduce_complete); release_event(stream_unmap_complete);
        cl_event label_unmap_complete{nullptr};
        check_opencl(clEnqueueUnmapMemObject(queue, stream_input_buffer, mapped_stream_values,
                                             0, nullptr, &stream_unmap_complete),
                     "unmapping GPU training features");
        check_opencl(clEnqueueUnmapMemObject(queue, stream_label_buffer, mapped_stream_labels,
                                             0, nullptr, &label_unmap_complete),
                     "unmapping GPU training labels");
        mapped_stream_values = nullptr; mapped_stream_labels = nullptr; mapped_stream_rows = 0U;
        check_opencl(clSetKernelArg(training_kernel, 0, sizeof(stream_input_buffer), &stream_input_buffer), "setting training features");
        check_opencl(clSetKernelArg(training_kernel, 1, sizeof(stream_label_buffer), &stream_label_buffer), "setting training labels");
        check_opencl(clSetKernelArg(training_kernel, 2, sizeof(rows_to_train), &rows_to_train), "setting training row count");
        check_opencl(clSetKernelArg(training_kernel, 3, sizeof(stream_update_buffer), &stream_update_buffer), "setting regression model state");
        check_opencl(clSetKernelArg(training_kernel, 4, sizeof(stream_gradient_buffer), &stream_gradient_buffer), "setting regression gradients");
        check_opencl(clSetKernelArg(training_kernel, 5, sizeof(stream_loss_buffer), &stream_loss_buffer), "setting regression losses");
        check_opencl(clSetKernelArg(training_kernel, 6, sizeof(stream_correct_buffer), &stream_correct_buffer), "setting regression accuracy");
        const cl_event waits[]{stream_unmap_complete, label_unmap_complete};
        check_opencl(clEnqueueNDRangeKernel(queue, training_kernel, 1, nullptr, &row_global_size, nullptr,
                                            2, waits, &stream_kernel_complete), "running parallel GPU regression rows");
        clReleaseEvent(label_unmap_complete);
        check_opencl(clSetKernelArg(training_reduce_kernel, 0, sizeof(stream_gradient_buffer), &stream_gradient_buffer), "setting regression reduction gradients");
        check_opencl(clSetKernelArg(training_reduce_kernel, 1, sizeof(stream_loss_buffer), &stream_loss_buffer), "setting regression reduction losses");
        check_opencl(clSetKernelArg(training_reduce_kernel, 2, sizeof(stream_correct_buffer), &stream_correct_buffer), "setting regression reduction accuracy");
        check_opencl(clSetKernelArg(training_reduce_kernel, 3, sizeof(rows_to_train), &rows_to_train), "setting regression reduction rows");
        check_opencl(clSetKernelArg(training_reduce_kernel, 4, sizeof(learning_rate_q16), &learning_rate_q16), "setting regression learning rate");
        check_opencl(clSetKernelArg(training_reduce_kernel, 5, sizeof(l2_q16), &l2_q16), "setting regression L2 regularisation");
        check_opencl(clSetKernelArg(training_reduce_kernel, 6, sizeof(stream_update_buffer), &stream_update_buffer), "setting regression output model");
        check_opencl(clSetKernelArg(training_reduce_kernel, 7, sizeof(stream_metrics_buffer), &stream_metrics_buffer), "setting regression metrics");
        constexpr std::size_t reduce_global_size{8U};
        check_opencl(clEnqueueNDRangeKernel(queue, training_reduce_kernel, 1, nullptr, &reduce_global_size, nullptr,
                                            1, &stream_kernel_complete, &stream_reduce_complete), "reducing GPU regression batch");
        check_opencl(clEnqueueReadBuffer(queue, stream_metrics_buffer, CL_FALSE, 0, stream_training_metrics.size() * sizeof(std::int64_t), stream_training_metrics.data(), 1, &stream_kernel_complete, nullptr), "reading GPU regression metrics");
        check_opencl(clEnqueueReadBuffer(queue, stream_update_buffer, CL_FALSE, 0,
                                         stream_update_values.size() * sizeof(std::int32_t), stream_update_values.data(),
                                         1, &stream_reduce_complete, &stream_read_complete), "reading GPU trained model");
        check_opencl(clFlush(queue), "starting GPU linear training");
        pending_stream_update_version = version;
        training_started = std::chrono::steady_clock::now();
    }

    [[nodiscard]] static double event_ms(cl_event event) noexcept {
        cl_ulong start{}; cl_ulong end{};
        if (event == nullptr || clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_START, sizeof(start), &start, nullptr) != CL_SUCCESS ||
            clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_END, sizeof(end), &end, nullptr) != CL_SUCCESS || end < start) return 0.0;
        return static_cast<double>(end - start) / 1'000'000.0;
    }

    [[nodiscard]] std::optional<TrainingUpdate> poll_training_update() {
        if (!pending_stream_update_version) return std::nullopt;
        cl_int status = CL_QUEUED;
        check_opencl(clGetEventInfo(stream_read_complete, CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof(status), &status, nullptr),
                     "polling GPU regression update");
        if (status < 0) throw std::runtime_error("GPU regression update failed (OpenCL error " + std::to_string(status) + ")");
        if (status != CL_COMPLETE) return std::nullopt;
        TrainingUpdate result{};
        result.model.update_version = *pending_stream_update_version;
        for (std::size_t index = 0; index < result.model.weights.size(); ++index) result.model.weights[index] = stream_update_values[index];
        result.model.buy_threshold = stream_update_values[result.model.weights.size()];
        result.model.sell_threshold = stream_update_values[result.model.weights.size() + 1U];
        result.metrics = {.squared_error_sum_q16 = stream_training_metrics[0],
                          .correct_predictions = static_cast<std::uint64_t>(stream_training_metrics[1]),
                          .rows = static_cast<std::uint64_t>(stream_training_metrics[2])};
        result.timing = {.upload_ms = event_ms(stream_unmap_complete),
                         .kernel_ms = event_ms(stream_kernel_complete) + event_ms(stream_reduce_complete),
                         .readback_ms = event_ms(stream_read_complete),
                         .end_to_end_ms = training_started ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *training_started).count() : 0.0};
        release_event(stream_read_complete); release_event(stream_reduce_complete); release_event(stream_kernel_complete); release_event(stream_unmap_complete);
        pending_stream_update_version.reset(); training_started.reset();
        return result;
    }

    void discard_training_batch() {
        if (mapped_stream_values == nullptr && mapped_stream_labels == nullptr) return;
        discard_stream_feature_rows();
        if (mapped_stream_labels != nullptr) {
            cl_event event{nullptr};
            check_opencl(clEnqueueUnmapMemObject(queue, stream_label_buffer, mapped_stream_labels,
                                                 0, nullptr, &event), "discarding mapped GPU training labels");
            mapped_stream_labels = nullptr;
            check_opencl(clWaitForEvents(1, &event), "waiting to discard GPU training labels");
            release_event(event);
        }
    }

private:
    // Release one optional OpenCL event and clear its handle.
    static void release_event(cl_event& event) noexcept {
        if (event != nullptr) {
            clReleaseEvent(event);
            event = nullptr;
        }
    }
#else
    // Keep the selected-device data valid in builds where OpenCL is unavailable.
    explicit Impl(app::OpenclDeviceInfo device) : selected_device(std::move(device)) {}
#endif
};

// Select the requested GPU and create its reusable OpenCL implementation.
GpuModel::GpuModel(std::optional<std::uint32_t> gpu_index, std::optional<std::string_view> gpu_name)
    : impl_(std::make_unique<Impl>(app::select_opencl_gpu(gpu_index, gpu_name))) {}

// The implementation destructor releases the owned OpenCL resources.
GpuModel::~GpuModel() = default;
// Move ownership of the GPU connection into this model.
GpuModel::GpuModel(GpuModel&&) noexcept = default;
// Replace this model's GPU connection with one owned by another model.
GpuModel& GpuModel::operator=(GpuModel&&) noexcept = default;

// Return the human-readable details of the GPU selected during construction.
const app::OpenclDeviceInfo& GpuModel::device() const noexcept {
    return impl_->selected_device;
}

void GpuModel::set_training_model(const market::ModelParameters& model) {
#if !MARKET_ENGINE_HAS_OPENCL
    static_cast<void>(model);
    throw app::OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    impl_->set_training_model(model);
#endif
}

// Run the reusable doubling kernel, or explain that this build has no OpenCL support.
std::vector<float> GpuModel::double_values(std::span<const float> input) {
#if !MARKET_ENGINE_HAS_OPENCL
    (void)input;
    throw app::OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    return impl_->double_values(input);
#endif
}

std::span<std::int32_t> GpuModel::map_stream_feature_rows(const std::size_t rows) {
#if !MARKET_ENGINE_HAS_OPENCL
    static_cast<void>(rows);
    throw app::OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    return impl_->map_stream_feature_rows(rows);
#endif
}

void GpuModel::submit_phase6_model_update(const std::uint64_t version) {
#if !MARKET_ENGINE_HAS_OPENCL
    static_cast<void>(version);
    throw app::OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    impl_->submit_phase6_model_update(version);
#endif
}

std::optional<ModelUpdate> GpuModel::poll_phase6_model_update() {
#if !MARKET_ENGINE_HAS_OPENCL
    throw app::OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    return impl_->poll_phase6_model_update();
#endif
}

void GpuModel::discard_stream_feature_rows() {
#if !MARKET_ENGINE_HAS_OPENCL
    throw app::OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    impl_->discard_stream_feature_rows();
#endif
}

MappedTrainingBatch GpuModel::map_training_batch(const std::size_t rows) {
#if !MARKET_ENGINE_HAS_OPENCL
    static_cast<void>(rows);
    throw app::OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    return impl_->map_training_batch(rows);
#endif
}

void GpuModel::submit_training_batch(const std::uint64_t version, const std::int32_t learning_rate_q16, const std::int32_t l2_q16) {
#if !MARKET_ENGINE_HAS_OPENCL
    static_cast<void>(version); static_cast<void>(learning_rate_q16); static_cast<void>(l2_q16);
    throw app::OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    impl_->submit_training_batch(version, learning_rate_q16, l2_q16);
#endif
}

std::optional<TrainingUpdate> GpuModel::poll_training_update() {
#if !MARKET_ENGINE_HAS_OPENCL
    throw app::OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    return impl_->poll_training_update();
#endif
}

void GpuModel::discard_training_batch() {
#if !MARKET_ENGINE_HAS_OPENCL
    throw app::OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    impl_->discard_training_batch();
#endif
}

// Run a small known-answer GPU health check: [1, 2, 3] must become [2, 4, 6].
// Return passed, skipped, or failed rather than letting command-line callers
// need to translate OpenCL exceptions themselves.
GpuSmokeTestResult run_gpu_smoke_test(std::optional<std::uint32_t> gpu_index,
                                      std::optional<std::string_view> gpu_name) {
    try {
        GpuModel model(gpu_index, gpu_name);
        // A short input makes errors easy to display and verifies every data path.
        constexpr std::array<float, 3> input{1.0F, 2.0F, 3.0F};
        constexpr std::array<float, 3> expected{2.0F, 4.0F, 6.0F};
        GpuSmokeTestResult result;
        result.device = model.device();
        result.output = model.double_values(input);
        // A correct kernel must preserve the input length.
        if (result.output.size() != expected.size()) {
            result.message = "GPU returned the wrong number of smoke-test values";
            return result;
        }
        // Compare floats with a small tolerance rather than exact equality.
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (std::fabs(result.output[index] - expected[index]) > 0.0001F) {
                result.message = "GPU smoke-test output did not match [2, 4, 6]";
                return result;
            }
        }
        result.status = GpuSmokeTestStatus::passed;
        result.message = "GPU returned [2, 4, 6] for input [1, 2, 3]";
        return result;
    } catch (const app::OpenclSelectionError& error) {
        // A laptop with no requested GPU can skip this hardware-only test. An
        // explicitly requested index/name, however, is a real user error and
        // must not be mistaken for a successful or skipped selection.
        return {.status = (gpu_index || gpu_name) ? GpuSmokeTestStatus::failed : GpuSmokeTestStatus::skipped,
                .message = error.what()};
    } catch (const std::exception& error) {
        return {.status = GpuSmokeTestStatus::failed, .message = error.what()};
    }
}

}  // namespace market_engine::gpu
