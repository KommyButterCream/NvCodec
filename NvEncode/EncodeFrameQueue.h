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

	// frameCount 는 2 의 n 승이며 최소 2 여야 한다.
	// 1 이면 HELD 슬롯이 있는 동안 빈 슬롯이 없어 모든 EnqueueLatest 가 실패한다.
	//
	// 2 보다 큰 값은 쓸 수 없는 슬롯을 할당하는 낭비다.
	// 이 큐는 latest-only 라서 EnqueueLatest 가 항상 이전 프레임을 버리고
	// queuedCount 를 1 로 만든다. 따라서 어느 시점에도 QUEUED 1 개 +
	// HELD 1 개, 즉 2 슬롯만 사용한다.
	//
	// 재호출 가능하다. 다만 리더 스레드가 정지한 뒤에 호출해야 한다.
	// (AcquireReadFrame 이 반환한 포인터가 무효화되므로)
	bool Initialize(uint32_t frameCount, ReleaseFrameCallback releaseCallback, void* userData);

	// 큐를 닫고 대기 중인 리더를 깨운다. Queued 상태 프레임은 생산자에게 반납한다.
	// 리더 스레드가 아직 살아있을 수 있으므로 저장 공간은 해제하지 않는다.
	void Shutdown();

	bool EnqueueLatest(const InputFrameHandle& frameHandle, bool forceKeyFrame);
	EncodeFrameItem* AcquireReadFrame();
	void ReleaseReadFrame();

	// AcquireReadFrame 이 nullptr 을 반환했을 때 "큐가 닫혔다"와
	// "HELD 프레임이 남아있다(프로그래밍 오류)"를 구분하기 위해 사용한다.
	bool IsRunning() const;

	uint32_t GetDropCount() const;
	uint32_t GetProcessCount() const;

private:
	void ReleaseFrameHandle(InputFrameHandle& frameHandle);
	void DropQueuedFrames_NoLock();
	void ReleaseAllSlots_NoLock();
	void FreeStorage_NoLock();
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
	alignas(4) volatile LONG m_hasHeldFrame = FALSE;

	alignas(4) volatile LONG m_running = FALSE;
	alignas(4) volatile LONG m_dropCount = 0;
	alignas(4) volatile LONG m_processCount = 0;

	ReleaseFrameCallback m_releaseCallback = nullptr;
	void* m_releaseCallbackUserData = nullptr;

	SRWLOCK m_lock = SRWLOCK_INIT;
	CONDITION_VARIABLE m_cv = CONDITION_VARIABLE_INIT;
};
