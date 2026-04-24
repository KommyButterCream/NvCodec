#include "pch.h"
#include "BitstreamRingBuffer.h"

#include <malloc.h> // for _aligned_malloc, _aligned_free
#include <assert.h> // for assert

namespace
{
	inline bool IsPowerOfTwo(size_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	inline size_t WrapRingIndex(size_t index, size_t bufferCount)
	{
		return index & (bufferCount - 1);
	}

	inline size_t AlignUp(size_t memorySize, size_t alignment)
	{
		return (memorySize + (alignment - 1)) & ~(alignment - 1);
	}

	// BitstreamRingBuffer 클래스와 무관한 헬퍼 함수 정의 (stateless)
	inline size_t FindNextSlotWithState(const BitstreamRingBuffer::SlotState* states, size_t bufferCount, size_t start, BitstreamRingBuffer::SlotState state)
	{
		for (size_t i = 0; i < bufferCount; ++i)
		{
			const size_t index = (start + i) & (bufferCount - 1);
			if (states[index] == state)
			{
				return index;
			}
		}

		return bufferCount;
	}
}

// 고정된 크기, 고정된 수량의 버퍼를 초기화 시점에 할당 후 재사용 한다.
BitstreamRingBuffer::BitstreamRingBuffer(size_t bufferSize, size_t bufferCount)
	: m_bufferCount(bufferCount)
{
	if (bufferSize == 0 || bufferCount == 0)
	{
		return;
	}

	if (!IsPowerOfTwo(m_bufferCount))
	{
		assert(false && "bufferCount must be a power of two");

		return;
	}

	// 64바이트 정렬이 수행된 크기 계산
	m_bufferSize = AlignUp(bufferSize, 64);
	const size_t totalSize = m_bufferSize * m_bufferCount;

	// 64바이트 정렬된 버퍼 수량 x 버퍼 크기 만큼의 1D Raw Buffer 할당 받은 후
	// 버퍼 크기만큼 나누어서 Array 처럼 사용한다.
	// Cache Line, SIMD, memcpy 최적화 목적
	// _aligned_malloc 는 _aligned_free 로 해제 해주어야 한다.
	m_buffers = static_cast<uint8_t*>(_aligned_malloc(totalSize, 64));
	if (!m_buffers)
	{
		m_bufferCount = 0;
		m_bufferSize = 0;
		return;
	}

	// 버퍼 수량 만큼의 Encoding 된 데이터를 보관하는 EncodePacket 생성
	// 1D Raw Buffer 를 Array 처럼 나누어서 사용될 것이다.
	m_packets = new EncodedPacket[m_bufferCount]();
	if (!m_packets)
	{
		_aligned_free(m_buffers);
		m_buffers = nullptr;
		m_bufferCount = 0;
		m_bufferSize = 0;
		return;
	}

	// 버퍼 수량 만큼의 EncodedPacket 상태를 나타내는 SlotState 생성
	// Free, Queued, Held 3가지 상태 보유
	m_states = new SlotState[m_bufferCount]();
	if (!m_states)
	{
		delete[] m_packets;
		m_packets = nullptr;
		_aligned_free(m_buffers);
		m_buffers = nullptr;
		m_bufferCount = 0;
		m_bufferSize = 0;
		return;
	}

	// 1D Raw Buffer 를 나누어 EncodedPacket 의 Data Buffer 로 사용한다.
	// 고정된 크기를 가지며 수량은 버퍼 카운트 만큼만 고정된 수량을 사용.
	// 버퍼가 꽉 차면 기존 데이터를 Drop 하고 새로운 데이터로 Overwrite 하는 구조.
	// EncodedPacket 은 자기 슬롯의 메모리(data) 만 바라본다.
	for (size_t i = 0; i < m_bufferCount; ++i)
	{
		m_packets[i].data = m_buffers + (m_bufferSize * i);
		m_packets[i].size = 0;
		m_states[i] = SLOT_FREE;
	}
}

BitstreamRingBuffer::~BitstreamRingBuffer()
{
	// 대기 중인 스레드에 종료 이벤트 통지 및 대기
	Shutdown();

	if (m_states)
	{
		delete[] m_states;
		m_states = nullptr;
	}

	if (m_packets)
	{
		delete[] m_packets;
		m_packets = nullptr;
	}

	if (m_buffers)
	{
		_aligned_free(m_buffers);
		m_buffers = nullptr;
	}
}

bool BitstreamRingBuffer::EnqueuePacket(const uint8_t* data, size_t size)
{
	// NVENC 로 Encode 된 Packet Data 를 Decoding 하기 위한 Waiting Queue 에 Enqueue 한다.

	if (!m_packets || !m_buffers || !m_states || (!data && size > 0) || size > m_bufferSize)
	{
		return false;
	}

	// Shutdown 호출되어 종료 상태 중이면 실패 처리
	if (::InterlockedCompareExchange(&m_running, TRUE, TRUE) == FALSE)
	{
		return false;
	}

	::AcquireSRWLockExclusive(&m_lock);

	const bool wasEmpty = (m_queuedCount == 0);
	const bool hasHeldPacket = (::InterlockedCompareExchange(&m_hasHeldPacket, FALSE, FALSE) == TRUE);
	const size_t occupiedCount = m_queuedCount + (hasHeldPacket ? 1 : 0); // 큐에 쌓인 수량 + 현재 처리 중인 패킷

	if (occupiedCount >= m_bufferCount)
	{
		// 패킷 버퍼 꽉 찼을 때

		if (m_queuedCount == 0)
		{
			::ReleaseSRWLockExclusive(&m_lock);
			return false;
		}

		// 현재 ReadPos 에 있는 것이 가장 오래된 Packet 일 것이다.
		// 해당 인덱스를 Drop 시킨다.
		// 슬롯 상태 변경 후 Queued 되어 있는 Next Slot 으로 ReadPos 이동
		const size_t droppedIndex = m_readPos;
		m_states[droppedIndex] = SLOT_FREE;
		m_packets[droppedIndex].size = 0;
		m_readPos = FindNextSlotWithState(m_states, m_bufferCount, WrapRingIndex(droppedIndex + 1, m_bufferCount), SLOT_QUEUED);

		// Drop 했으므로 QueuedCount 감소, DropCount 증가.
		--m_queuedCount;
		++m_dropCount;

		// DropIndex 정리 완료, Write 가능.
		m_writePos = droppedIndex;
	}
	else
	{
		// 패킷 버퍼 여유 있을 때
		// 비어있는 Next Slot Packet Index 를 찾는다.
		const size_t freeIndex = FindNextSlotWithState(m_states, m_bufferCount, m_writePos, SLOT_FREE);
		if (freeIndex >= m_bufferCount)
		{
			::ReleaseSRWLockExclusive(&m_lock);
			return false;
		}

		// WritePos 설정
		m_writePos = freeIndex;
	}

	// 계산된 WritePos 로 EncodedPacket 접근하여 Write 한다.
	EncodedPacket& packet = m_packets[m_writePos];
	packet.size = size;
	if (size > 0)
	{
		// 입력 받은 데이터는 Nvidia 디코더 내부 버퍼의 포인터이므로
		// 포인터를 저장하면 안되고 내용 전체를 복사해서 가지고 있어야 한다.
		// Zero-Copy 구조를 일부로 사용하지 않음.
		memcpy(packet.data, data, size);
	}

	// 해당 Slot 은 Queued 되었음을 설정
	m_states[m_writePos] = SLOT_QUEUED;
	if (m_queuedCount == 0)
	{
		// Queued 된 EncodedPacket 이 없는 경우
		// PacketRead 시점에 데이터를 읽을 수 있도록
		// ReadPos 설정
		m_readPos = m_writePos;
	}

	// Enqueue 종료 시점이므로 Queued Count 수량 증가
	++m_queuedCount;

	// Next Pos 로 WritePos 이동
	m_writePos = WrapRingIndex(m_writePos + 1, m_bufferCount);

	if (wasEmpty)
	{
		// Empty 상태면 Consumer(AcquireReadPacket) 를 깨운다.
		::WakeConditionVariable(&m_cv);
	}

	::ReleaseSRWLockExclusive(&m_lock);

	return true;
}

BitstreamRingBuffer::EncodedPacket* BitstreamRingBuffer::AcquireReadPacket()
{
	// Decode Thread 가 Packet 을 하나 가져간다.

	if (!m_packets || !m_buffers || !m_states)
	{
		return nullptr;
	}

	::AcquireSRWLockExclusive(&m_lock);
	while (m_queuedCount == 0)
	{
		// Decode Thread 에서 데이터를 가져가려고 하였으나
		// Queued 된 Packet 이 없는 경우
		// Lock 을 풀고 Sleep 상태로 전환
		if (::InterlockedCompareExchange(&m_running, TRUE, TRUE) == FALSE)
		{
			// Shutdown 호출되어 종료 상태 중이면 nullptr 반환
			::ReleaseSRWLockExclusive(&m_lock);
			return nullptr;
		}

		::SleepConditionVariableSRW(&m_cv, &m_lock, INFINITE, 0);
	}

	// 중복 Decode 를 방지하기 위해 현재 Held 되어서 처리 중인 Packet 이 있는 경우를 방지
	if (::InterlockedCompareExchange(&m_hasHeldPacket, TRUE, TRUE) == TRUE)
	{
		::ReleaseSRWLockExclusive(&m_lock);
		return nullptr;
	}

	// ReadPos 로부터 EncodedPacket 획득 및 처리
	const size_t heldIndex = m_readPos;
	m_states[heldIndex] = SLOT_HELD;
	m_heldPos = heldIndex;

	// 현재 Decode Thread 에서 Packet 을 하나 처리 중임을 알림
	::InterlockedExchange(&m_hasHeldPacket, TRUE);

	// Queued Packet Count 감소
	--m_queuedCount;

	if (m_queuedCount > 0)
	{
		// Queued Packet 이 남아 있는 경우라면
		// Next Queued Index 를 찾아서 ReadPos 로 설정하여 다음 패킷이 처리되도록 한다.
		m_readPos = FindNextSlotWithState(m_states, m_bufferCount, WrapRingIndex(heldIndex + 1, m_bufferCount), SLOT_QUEUED);
	}
	else
	{
		// 처리할 Packet 이 없는 경우 ReadPos 단순 1 증가
		// 해당 Index 에 새로운 Packet 이 Enqueue 될 것이다.
		m_readPos = WrapRingIndex(heldIndex + 1, m_bufferCount);
	}

	// Queued 된 EncodedPacket 을 반환하여
	// 디코딩을 수행시킨다.
	EncodedPacket* packet = &m_packets[heldIndex];
	::ReleaseSRWLockExclusive(&m_lock);

	return packet;
}

void BitstreamRingBuffer::ReleaseReadPacket()
{
	if (!m_packets || !m_states)
	{
		return;
	}

	// Decode 가 끝날 때 까지 HeldPos 의 Packet 은 Free 상태로 변경하지 않는다.
	// 이 함수가 호출될 시점에는 정상적인 경우라면 hasHeldPacket 이 TRUE 일 것이다.
	// Decode 가 끝나서 해당 Slot 을 비워 준다.
	::AcquireSRWLockExclusive(&m_lock);
	if (::InterlockedCompareExchange(&m_hasHeldPacket, TRUE, TRUE) == TRUE)
	{
		m_states[m_heldPos] = SLOT_FREE;
		m_packets[m_heldPos].size = 0;
		::InterlockedExchange(&m_hasHeldPacket, FALSE);
		::InterlockedIncrement(&m_processCount);
	}
	::ReleaseSRWLockExclusive(&m_lock);
}

void BitstreamRingBuffer::Shutdown()
{
	// 대기 중인 Decode Thread 에 종료 이벤트 통지 및 대기
	::InterlockedExchange(&m_running, FALSE);

	::AcquireSRWLockExclusive(&m_lock);
	::WakeAllConditionVariable(&m_cv);
	::ReleaseSRWLockExclusive(&m_lock);
}

int32_t BitstreamRingBuffer::GetProcessCount()
{
	// 처리 완료된 Packet Count 수량 반환
	return ::InterlockedCompareExchange(&m_processCount, 0, 0);
}

size_t BitstreamRingBuffer::GetBufferSize() const
{
	return m_bufferSize;
}
