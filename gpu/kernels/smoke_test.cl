// Phase 6.4 OpenCL health-check kernel.
// Each GPU work item doubles exactly one input value and writes it to the same
// position in the output buffer. It is intentionally independent of the market
// model so it proves the OpenCL connection before Phase 6.5 adds data schemas.
__kernel void double_values(__global const float* input,
                            __global float* output,
                            const uint count) {
    const uint index = get_global_id(0);
    if (index < count) {
        output[index] = input[index] * 2.0F;
    }
}

// Phase 6 streaming proof kernel. It receives the contiguous `[N][8]` Q16.16
// feature memory filled directly by GpuWorker and returns one complete absolute
// model replacement. This is deliberately not learning: Phase 7 replaces only
// this implementation with regression/training kernels while keeping the same
// ModelUpdate boundary. Weight zero echoes the first feature so GPU execution
// demonstrably consumed the submitted batch; the remaining values are known.
__kernel void phase6_model_update(__global const int* features,
                                  __global int* model_values,
                                  const uint feature_count) {
    if (get_global_id(0) != 0) return;

    model_values[0] = feature_count == 0 ? 0 : features[0];
    model_values[1] = 0;
    model_values[2] = 0;
    model_values[3] = 0;
    model_values[4] = 0;
    model_values[5] = 0;
    model_values[6] = 0;
    model_values[7] = 65536;   // Q16.16 1.0 bias/intercept.
    model_values[8] = 0;       // Absolute BUY threshold.
    model_values[9] = -65536;  // Absolute SELL threshold.
}

// Proper batch regression: every row is evaluated independently.  This is the
// expensive part of training and launches one work item per row, so a batch is
// genuinely data-parallel rather than serial SGD disguised as a GPU kernel.
__kernel void regression_row_gradients(__global const int* features,
                                       __global const int* labels,
                                       const uint rows,
                                       __global const int* model_values,
                                       __global long* row_gradients,
                                       __global long* row_loss,
                                       __global int* row_correct) {
    const uint row = get_global_id(0);
    if (row >= rows) return;
    long score = 0;
    for (uint feature = 0; feature < 8; ++feature)
        score += ((long)model_values[feature] * features[row * 8 + feature]) >> 16;
    const long error = (long)labels[row] - score;
    for (uint feature = 0; feature < 8; ++feature)
        row_gradients[row * 8 + feature] = (error * (long)features[row * 8 + feature]) >> 16;
    row_loss[row] = (error * error) >> 16;
    const int prediction = score > 0 ? 65536 : (score < 0 ? -65536 : 0);
    row_correct[row] = prediction == labels[row] ? 1 : 0;
}

// The second phase reduces row gradients into a mean batch gradient, applies
// L2 regularisation, and updates the persistent device model. Eight work items
// update independent coefficients; row evaluation above provides the parallel
// throughput, while this compact deterministic reduction fixes update order.
__kernel void regression_apply_batch(__global const long* row_gradients,
                                     __global const long* row_loss,
                                     __global const int* row_correct,
                                     const uint rows,
                                     const int learning_rate_q16,
                                     const int l2_q16,
                                     __global int* model_values,
                                     __global long* metrics) {
    const uint feature = get_global_id(0);
    if (feature >= 8) return;
    long total_gradient = 0;
    for (uint row = 0; row < rows; ++row) total_gradient += row_gradients[row * 8 + feature];
    const long mean_gradient = total_gradient / (long)rows;
    const long penalty = ((long)l2_q16 * model_values[feature]) >> 16;
    long next = (long)model_values[feature] + (((long)learning_rate_q16 * (mean_gradient - penalty)) >> 16);
    if (next > 2097152) next = 2097152;
    if (next < -2097152) next = -2097152;
    model_values[feature] = (int)next;
    if (feature == 0) {
        long loss = 0; long correct = 0;
        for (uint row = 0; row < rows; ++row) { loss += row_loss[row]; correct += row_correct[row]; }
        metrics[0] = loss; metrics[1] = correct; metrics[2] = rows;
        if (model_values[8] <= model_values[9]) { model_values[8] = 16384; model_values[9] = -16384; }
    }
}
