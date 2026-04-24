#pragma once

#include <cstdint>

struct NvEncPacket
{
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};