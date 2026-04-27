#pragma once

#include <stdint.h>

struct NvEncPacket
{
	const uint8_t* data = nullptr;
	uint32_t size = 0;
};