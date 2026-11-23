// Kernels the compute test uses. Build: userland/kernels/build.sh (on the Mac, Metal Toolchain installed).
#include <metal_stdlib>
using namespace metal;

// out[i] = i * 0x9e3779b1 + seed
kernel void fill_id(device uint *out [[buffer(0)]], constant uint &seed [[buffer(1)]], uint id [[thread_position_in_grid]])
{
	out[id] = id * 0x9e3779b1u + seed;
}

// y[i] = a * x[i] + y[i]
kernel void saxpy(device const float *x [[buffer(0)]], device float *y [[buffer(1)]], constant float &a [[buffer(2)]],
    uint id [[thread_position_in_grid]])
{
	y[id] = a * x[id] + y[id];
}

// out[i] = in[i] squared, as integers
kernel void square(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{
	out[id] = in[id] * in[id];
}
