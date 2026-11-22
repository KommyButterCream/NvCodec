#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <stdint.h>
#include <stddef.h>

#include "NvDecConfig.h"

#ifndef D3D11_NVIDIA_DECODER_API
#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define D3D11_NVIDIA_DECODER_API __declspec(dllexport)
#else
#define D3D11_NVIDIA_DECODER_API __declspec(dllimport)
#endif
#endif

class D3D11_NVIDIA_DECODER_API DecodePacketQueue
{
public:
	// 공개 입력 타입과 같은 것이다. 앱은 NvDecPacket 이라는 이름만 알면 된다.
	using InputPacket = NvDecPacket;

	struct DecodePacketItem
	{
		uint8_t* data = nullptr;
		size_t size = 0;
		uint64_t timestamp = 0;
	};

	enum SlotState : uint8_t
	{
		SLOT_FREE = 0,
		SLOT_QUEUED,
		SLOT_HELD,
	};

public:
	DecodePacketQueue() = default;
	~DecodePacketQueue();

	// EncodeFrameQueue 와 같은 2 단계 초기화다. 실패는 반환값으로 알린다.
	bool Initialize(size_t packetCount, size_t bufferSize);

	bool EnqueuePacket(const InputPacket& packet);
	DecodePacketItem* AcquireReadPacket();
	void ReleaseReadPacket();

	void Shutdown();

	// AcquireReadPacket 이 nullptr 을 반환했을 때 "큐가 닫혔다"와
	// "HELD 패킷이 남아있다(프로그래밍 오류)"를 구분하기 위해 사용한다.
	bool IsRunning() const;

	uint32_t GetDequeuedCount() const;

	// 뒤에 온 패킷에 밀려 버려진 수. 관측이 안 되면 왜 끊기는지 알 수 없다.
	uint64_t GetDropCount() const;

	size_t GetBufferSize() const;

private:
	size_t m_packetCount = 0;
	size_t m_bufferSize = 0;
	uint8_t* m_buffers = nullptr;
	DecodePacketItem* m_items = nullptr;
	SlotState* m_states = nullptr;

	// =====================================================================
	// m_lock 이 지키는 큐 인덱스
	// =====================================================================
	alignas(64) size_t m_writePos = 0;
	size_t m_readPos = 0;
	size_t m_queuedCount = 0;
	size_t m_heldPos = 0;
	volatile LONG m_hasHeldPacket = FALSE;

	// =====================================================================
	// 락 없이 읽히는 상태 / 카운터
	// =====================================================================
	alignas(64) volatile LONG m_running = FALSE;
	volatile LONG64 m_dropCount = 0;
	volatile LONG m_dequeuedCount = 0;

	SRWLOCK m_lock = SRWLOCK_INIT;
	CONDITION_VARIABLE m_cv = CONDITION_VARIABLE_INIT;
};
