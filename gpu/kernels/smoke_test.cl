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
