#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <stdint.h>
#include <stddef.h>

#ifndef D3D11_NVIDIA_DECODER_API
#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define D3D11_NVIDIA_DECODER_API __declspec(dllexport)
#else
#define D3D11_NVIDIA_DECODER_API __declspec(dllimport)
#endif
#endif

class D3D11_NVIDIA_DECODER_API DecodeFrameQueue
{
public:
	struct InputFrameHandle
	{
		const uint8_t* data = nullptr;
		size_t size = 0;
		uint64_t frameId = 0;
		uint64_t timestamp = 0;
		uint16_t frameType = 0;
	};

	struct DecodeFrameItem
	{
		uint8_t* data = nullptr;
		size_t size = 0;
		uint64_t frameId = 0;
		uint64_t timestamp = 0;
		uint16_t frameType = 0;
	};

	enum SlotState : uint8_t
	{
		SLOT_FREE = 0,
		SLOT_QUEUED,
		SLOT_HELD,
	};

public:
	DecodeFrameQueue(size_t bufferSize, size_t bufferCount);
	~DecodeFrameQueue();

	bool EnqueueFrame(const InputFrameHandle& frameHandle);
	DecodeFrameItem* AcquireReadFrame();
	void ReleaseReadFrame();

	void Shutdown();

	int32_t GetProcessCount();
	size_t GetBufferSize() const;

private:
	size_t m_bufferCount = 0;
	size_t m_bufferSize = 0;

	size_t m_dropCount = 0;
	uint8_t* m_buffers = nullptr;
	DecodeFrameItem* m_items = nullptr;
	SlotState* m_states = nullptr;

	alignas(64) size_t m_writePos = 0;
	alignas(64) size_t m_readPos = 0;
	alignas(64) size_t m_queuedCount = 0;
	alignas(64) size_t m_heldPos = 0;
	volatile LONG m_hasHeldFrame = FALSE;

	volatile LONG m_running = TRUE;
	volatile LONG m_processCount = 0;

	SRWLOCK m_lock = SRWLOCK_INIT;
	CONDITION_VARIABLE m_cv = CONDITION_VARIABLE_INIT;
};
