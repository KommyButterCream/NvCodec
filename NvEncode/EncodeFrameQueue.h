#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <stdint.h>

#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define D3D11_NVIDIA_ENCODER_API __declspec(dllexport)
#else
#define D3D11_NVIDIA_ENCODER_API __declspec(dllimport)
#endif

struct ID3D11Texture2D;

class D3D11_NVIDIA_ENCODER_API EncodeFrameQueue
{
public:
	struct InputFrameHandle
	{
		ID3D11Texture2D* texture = nullptr;
		int64_t sourceSlotId = -1;
		uint64_t frameId = 0ULL;
	};

	struct EncodeFrameItem
	{
		InputFrameHandle frameHandle = {};
		bool forceKeyFrame = false;
	};

	using ReleaseFrameCallback = void (*)(InputFrameHandle& frameHandle, void* userData);

	enum SlotState : uint8_t
	{
		SLOT_FREE = 0,
		SLOT_QUEUED,
		SLOT_HELD,
	};

public:
	EncodeFrameQueue() = default;
	~EncodeFrameQueue();

	bool Initialize(uint32_t frameCount, ReleaseFrameCallback releaseCallback, void* userData);
	void Shutdown();

	bool EnqueueLatest(const InputFrameHandle& frameHandle, bool forceKeyFrame);
	EncodeFrameItem* AcquireReadFrame();
	void ReleaseReadFrame();

	uint32_t GetDropCount() const;
	uint32_t GetProcessCount() const;

private:
	void ReleaseFrameHandle(InputFrameHandle& frameHandle);
	void DropQueuedFrames_NoLock();
	uint32_t FindNextSlotWithState(uint32_t start, SlotState state) const;
	uint32_t WrapIndex(uint32_t index) const;
	bool IsPowerOfTwo(uint32_t value) const;

private:
	uint32_t m_frameCount = 0;
	EncodeFrameItem* m_items = nullptr;
	SlotState* m_states = nullptr;

	alignas(64) uint32_t m_writePos = 0;
	alignas(64) uint32_t m_readPos = 0;
	alignas(64) uint32_t m_queuedCount = 0;
	alignas(64) uint32_t m_heldPos = 0;
	volatile LONG m_hasHeldFrame = FALSE;

	volatile LONG m_running = FALSE;
	volatile LONG m_dropCount = 0;
	volatile LONG m_processCount = 0;

	ReleaseFrameCallback m_releaseCallback = nullptr;
	void* m_releaseCallbackUserData = nullptr;

	SRWLOCK m_lock = SRWLOCK_INIT;
	CONDITION_VARIABLE m_cv = CONDITION_VARIABLE_INIT;
};
