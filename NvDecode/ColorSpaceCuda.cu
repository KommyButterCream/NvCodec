#include "ColorSpaceCuda.cuh"
#include <device_launch_parameters.h>

// Utility
__device__ __forceinline__ uint8_t clampToByte(int32_t value)
{
    return (value < 0) ? 0 : (value > 255 ? 255 : value);
}

// Kernel
__global__ void NV12ToBGRAKernel(
    const uint8_t* __restrict__ yPlane,
    const uint8_t* __restrict__ uvPlane,
    int32_t width,
    int32_t height,
    int32_t srcPitch,
    uchar4* __restrict__ dst,
    int32_t dstPitch)
{
    int32_t x = blockIdx.x * blockDim.x + threadIdx.x;
    int32_t y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) return;

    // 1. Y 값 읽기
    int32_t Y = yPlane[y * srcPitch + x];

    // 2. UV 값 읽기 (NV12: 인터리브드)
    int32_t uvIdx = (y >> 1) * srcPitch + (x & ~1); // x & ~1 은 (x >> 1) * 2 와 같음
    int32_t U = (int32_t)uvPlane[uvIdx] - 128;
    int32_t V = (int32_t)uvPlane[uvIdx + 1] - 128;

    // 3. BT.709 Full Range 정밀 계산
    int32_t R = Y + ((403 * V + 128) >> 8);
    int32_t G = Y - ((48 * U + 120 * V + 128) >> 8);
    int32_t B = Y + ((475 * U + 128) >> 8);

    // 4. 결과 조립 (BGRA 순서 체크)
    uchar4 out;
    out.x = clampToByte(B); // Blue
    out.y = clampToByte(G); // Green
    out.z = clampToByte(R); // Red
    out.w = 255;

    // 5. 메모리 쓰기 (Pitch 단위 안전 쓰기)
    uchar4* pDstLine = (uchar4*)((uint8_t*)dst + y * dstPitch);
    pDstLine[x] = out;
}

void ConvertNV12ToBGRA(uint8_t* srcY, uint8_t* srcUV, int32_t width, int32_t height, int32_t srcPitch, uchar4* dst, int32_t dstPitch, cudaStream_t stream)
{
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    NV12ToBGRAKernel << <grid, block, 0, stream >> > (
        srcY,
        srcUV,
        width,
        height,
        srcPitch,
        dst,
        dstPitch
        );
}
