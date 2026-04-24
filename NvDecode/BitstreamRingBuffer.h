#pragma once

// NvDecoder 로 디코딩 하기 위한 BitstreamBuffer 를 RingBuffer 형식으로 저장 및 관리
// 버퍼가 꽉 차면 oldest stream buffer 부터 drop 한다.
// 항상 1개의 Packet 만 Decode 중인 상태를 유지한다. (Single Decode Thread 사용을 가정)

#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define DECODER_BITSTREAM_RINGBUFFER_API __declspec(dllexport)
#else
#define DECODER_BITSTREAM_RINGBUFFER_API __declspec(dllimport)
#endif

class DECODER_BITSTREAM_RINGBUFFER_API BitstreamRingBuffer
{
public:
	struct EncodedPacket
	{
		uint8_t* data = nullptr;
		size_t size = 0;
	};

	enum SlotState : uint8_t
	{
		SLOT_FREE = 0, // 비어 있음
		SLOT_QUEUED,   // 버퍼에 Enqueue 후 대기 중
		SLOT_HELD,     // 현재 디코딩 중
	};

public:
	BitstreamRingBuffer(size_t bufferSize, size_t bufferCount);
	~BitstreamRingBuffer();

	bool EnqueuePacket(const uint8_t* data, size_t size);
	EncodedPacket* AcquireReadPacket();
	void ReleaseReadPacket();

	void Shutdown();

	int32_t GetProcessCount();
	size_t GetBufferSize() const;

private:
	size_t m_bufferCount = 0; // RingBuffer 생성에 사용되는 Encode 된 데이터 버퍼 수량
	size_t m_bufferSize = 0; // RingBuffer 생성에 사용되는 Encod 된 데이터 크기

	size_t m_dropCount = 0;
	uint8_t* m_buffers = nullptr; // 버퍼 수량 x 버퍼 크기 만큼의 1D Raw Buffer 할당
	EncodedPacket* m_packets = nullptr; // Encode 된 데이터 저장
	SlotState* m_states = nullptr; // EncodedPacket 상태 관리

	alignas(64) size_t m_writePos = 0;
	alignas(64) size_t m_readPos = 0;
	alignas(64) size_t m_queuedCount = 0;
	alignas(64) size_t m_heldPos = 0;
	volatile LONG m_hasHeldPacket = FALSE;

	volatile LONG m_running = TRUE;
	volatile LONG m_processCount = 0;

	SRWLOCK m_lock = SRWLOCK_INIT;
	CONDITION_VARIABLE m_cv = CONDITION_VARIABLE_INIT;
};
