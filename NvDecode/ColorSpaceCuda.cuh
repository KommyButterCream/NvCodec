#include <cuda.h>
#include <cuda_runtime.h>

#include <stdint.h>

void ConvertNV12ToBGRA(
    uint8_t* srcY,
    uint8_t* srcUV,
    int32_t width,
    int32_t height,
    int32_t srcPitch,
    uchar4* dst,
    int32_t dstPitch,
    cudaStream_t stream);

