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

// Phase 7 v1 learner. One work item performs deterministic Q16.16 SGD over a
// labelled batch and leaves the updated model resident in `model_values` for the
// next batch. This is intentionally simple before a later parallel optimiser;
// it is nevertheless real GPU-side training, not a CPU reference calculation.
__kernel void train_linear_model(__global const int* features,
                                 __global const int* labels,
                                 const uint rows,
                                 const int learning_rate_q16,
                                 __global int* model_values) {
    if (get_global_id(0) != 0) return;

    long weights[8];
    for (uint feature = 0; feature < 8; ++feature) weights[feature] = model_values[feature];

    for (uint row = 0; row < rows; ++row) {
        long score = 0;
        for (uint feature = 0; feature < 8; ++feature) {
            score += (weights[feature] * (long)features[row * 8 + feature]) >> 16;
        }
        const long error = (long)labels[row] - score;
        for (uint feature = 0; feature < 8; ++feature) {
            const long gradient = (error * (long)features[row * 8 + feature]) >> 16;
            weights[feature] += ((long)learning_rate_q16 * gradient) >> 16;
            if (weights[feature] > 2097152) weights[feature] = 2097152;
            if (weights[feature] < -2097152) weights[feature] = -2097152;
        }
    }

    long positive_scores = 0; uint positive_count = 0;
    long negative_scores = 0; uint negative_count = 0;
    for (uint row = 0; row < rows; ++row) {
        long score = 0;
        for (uint feature = 0; feature < 8; ++feature) {
            score += (weights[feature] * (long)features[row * 8 + feature]) >> 16;
        }
        if (labels[row] > 0) { positive_scores += score; ++positive_count; }
        if (labels[row] < 0) { negative_scores += score; ++negative_count; }
    }
    for (uint feature = 0; feature < 8; ++feature) model_values[feature] = (int)weights[feature];

    // Place each threshold halfway between HOLD (zero) and the learned average
    // score for that profitable direction. Preserve the prior value when this
    // batch contains no example of that direction.
    int buy = model_values[8];
    int sell = model_values[9];
    if (positive_count != 0) buy = (int)((positive_scores / (long)positive_count) / 2);
    if (negative_count != 0) sell = (int)((negative_scores / (long)negative_count) / 2);
    if (buy <= sell) { buy = 16384; sell = -16384; }
    model_values[8] = buy;
    model_values[9] = sell;
}
