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

// ---- render (M5.4) ----
struct TriOut {
	float4 pos [[position]];
	float4 color;
};

// three vertices from the vertex id alone, no vertex buffer
vertex TriOut tri_vs(uint vid [[vertex_id]])
{
	TriOut o;
	float x = vid == 1 ? 0.8 : (vid == 2 ? 0.0 : -0.8);
	float y = vid == 2 ? 0.8 : -0.8;
	o.pos = float4(x, y, 0.0, 1.0);
	o.color = float4(vid == 0 ? 1.0 : 0.0, vid == 1 ? 1.0 : 0.0, vid == 2 ? 1.0 : 0.0, 1.0);
	return o;
}

struct AttrIn {
	float2 pos [[attribute(0)]];
	float4 color [[attribute(1)]];
};

// the same from a vertex buffer: float2 position, float4 colour
vertex TriOut attr_vs(AttrIn in [[stage_in]])
{
	TriOut o;
	o.pos = float4(in.pos, 0.0, 1.0);
	o.color = in.color;
	return o;
}

fragment float4 tri_fs(TriOut in [[stage_in]])
{
	return in.color;
}
