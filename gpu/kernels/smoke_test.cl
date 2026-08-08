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
