#pragma once

#include <stdint.h>

struct NvEncPacket
{
	const uint8_t* data = nullptr;
	uint32_t size = 0;
	uint64_t frameId = 0;
	uint64_t timestamp = 0;
	uint16_t frameType = 0;
	bool isKeyFrame = false;
};
