#include "pch.h"
#include "DecodePacketQueue.h"

#include <malloc.h>
#include <assert.h>


namespace
{
	inline bool IsPowerOfTwo(size_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	inline size_t WrapRingIndex(size_t index, size_t bufferCount)
	{
		// 버퍼 수량이 2의 n 승을 보장하므로
		// Bit And 연산을 통해 Warp-Around.
		// '%' 연산보다 속도가 빠르다.
		return index & (bufferCount - 1);
	}

	inline size_t AlignUp(size_t memorySize, size_t alignment)
	{
		return (memorySize + (alignment - 1)) & ~(alignment - 1);
	}

	inline size_t FindNextSlotWithState(const DecodePacketQueue::SlotState* states, size_t bufferCount, size_t start, DecodePacketQueue::SlotState state)
	{
		// Start Index 부터 시작하여 입력받은 State 와 동일한 Slot Index 를 반환.
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

bool DecodePacketQueue::Initialize(size_t packetCount, size_t bufferSize)
{
	m_packetCount = packetCount;

	if (bufferSize == 0 || packetCount == 0)
	{
		m_packetCount = 0;
		return false;
	}

	// 버퍼 수량이 2 의 n 승일 것임을 보장 해야 한다.
	if (!IsPowerOfTwo(m_packetCount))
	{
		assert(false && "bufferCount must be a power of two");
		return false;
	}

	// 64 바이트 얼라인 된 버퍼 1개의 크기를 계산
	m_bufferSize = AlignUp(bufferSize, 64);

	// 필요로되는 전체 메모리 크기만큼 선형적 메모리를 할당
	const size_t totalSize = m_bufferSize * m_packetCount;

	// 디코딩 Raw 데이터 저장을 위한 1D Linear 버퍼 할당
	m_buffers = static_cast<uint8_t*>(_aligned_malloc(totalSize, 64));
	if (!m_buffers)
	{
		m_packetCount = 0;
		m_bufferSize = 0;
		return false;
	}

	// 디코딩 데이터를 저장할 구조체 버퍼 할당
	m_items = new DecodePacketItem[m_packetCount]();
	if (!m_items)
	{
		_aligned_free(m_buffers);
		m_buffers = nullptr;
		m_packetCount = 0;
		m_bufferSize = 0;
		return false;
	}

	// 동일 수량만큼의 Slot 상태 저장하는 버퍼 할당
	m_states = new SlotState[m_packetCount]();
	if (!m_states)
	{
		delete[] m_items;
		m_items = nullptr;
		_aligned_free(m_buffers);
		m_buffers = nullptr;
		m_packetCount = 0;
		m_bufferSize = 0;
		return false;
	}

	// 기본값 초기화
	for (size_t i = 0; i < m_packetCount; ++i)
	{
		m_items[i].data = m_buffers + (m_bufferSize * i);
		m_items[i].size = 0;
		m_items[i].timestamp = 0;
		m_states[i] = SLOT_FREE;
	}

	::InterlockedExchange(&m_running, TRUE);
	return true;
}

DecodePacketQueue::~DecodePacketQueue()
{

	Shutdown();

	if (m_states)
	{
		delete[] m_states;
		m_states = nullptr;
	}

	if (m_items)
	{
		delete[] m_items;
		m_items = nullptr;
	}

	if (m_buffers)
	{
		_aligned_free(m_buffers);
		m_buffers = nullptr;
	}
}

bool DecodePacketQueue::EnqueuePacket(const InputPacket& packet)
{
	// 패킷을 큐 버퍼에 복사해 저장한다.
	// 큐는 고정 크기 링 버퍼 형태이다.
	if (!m_items || !m_buffers || !m_states || (!packet.data && packet.size > 0) || packet.size > m_bufferSize)
	{
		return false;
	}

	// DecodePacketQueue 사용 중이 아니라면 종료
	if (::ReadAcquire(&m_running) == FALSE)
	{
		return false;
	}

	// m_items, m_states 접근을 위한 Lock
	::AcquireSRWLockExclusive(&m_lock);

	const bool wasEmpty = (m_queuedCount == 0);
	const bool hasHeldPacket = (::ReadAcquire(&m_hasHeldPacket) == TRUE);
	const size_t occupiedCount = m_queuedCount + (hasHeldPacket ? 1 : 0);

	// 패킷을 저장하기 위한 비어 있는 슬롯을 찾는다.
	if (occupiedCount >= m_packetCount)
	{
		// 모든 슬롯이 Queued 되어 있어서 사용할 공간이 없는 경우
		// Latest Read Pos 의 데이터를 drop 하고 해당 슬롯에
		// 데이터를 저장할 수 있도록 한다.
		if (m_queuedCount == 0)
		{
			::ReleaseSRWLockExclusive(&m_lock);
			return false;
		}

		const size_t droppedIndex = m_readPos;
		m_states[droppedIndex] = SLOT_FREE;
		m_items[droppedIndex].size = 0;
		m_items[droppedIndex].timestamp = 0;
		m_readPos = FindNextSlotWithState(m_states, m_packetCount, WrapRingIndex(droppedIndex + 1, m_packetCount), SLOT_QUEUED);

		--m_queuedCount;
		::InterlockedIncrement64(&m_dropCount);
		m_writePos = droppedIndex;
	}
	else
	{
		// 사용 가능한 슬롯이 있는 경우 FREE 상태인 슬롯 인덱스 반환
		const size_t freeIndex = FindNextSlotWithState(m_states, m_packetCount, m_writePos, SLOT_FREE);
		if (freeIndex >= m_packetCount)
		{
			::ReleaseSRWLockExclusive(&m_lock);
			return false;
		}

		m_writePos = freeIndex;
	}

	// 데이터 저장을 위한 슬롯 ID 계산이 끝났으므로 (m_writePos)
	// 해당 위치에 패킷 데이터를 저장한다.
	// 이때, Encode Raw Data 를 슬롯 버퍼로 Deep-Copy 복사하여 저장한다.
	DecodePacketItem& item = m_items[m_writePos];
	item.size = packet.size;
	item.timestamp = packet.timestamp;
	if (packet.size > 0)
	{
		memcpy(item.data, packet.data, packet.size);
	}

	// 해당 슬롯이 Queued 되어있음을 상태 저장
	m_states[m_writePos] = SLOT_QUEUED;
	
	// 처음 저장하는 경우에는 readpos == writepos 맞춰주도록 한다.
	if (m_queuedCount == 0)
	{
		m_readPos = m_writePos;
	}

	// Queued Item 수량을 증가시키고
	// 다음 Write Pos 를 업데이트 한다.
	++m_queuedCount;
	m_writePos = WrapRingIndex(m_writePos + 1, m_packetCount);

	// 데이터 처리 중이 아니라면 스레드를 깨워 작업을 시키기 위해
	// Condition_Variable 를 깨운다.
	if (wasEmpty)
	{
		::WakeConditionVariable(&m_cv);
	}

	::ReleaseSRWLockExclusive(&m_lock);

	return true;
}

DecodePacketQueue::DecodePacketItem* DecodePacketQueue::AcquireReadPacket()
{
	// 디코드 스레드 측에서 디코딩할 데이터를 획득하는 함수

	if (!m_items || !m_buffers || !m_states)
	{
		return nullptr;
	}

	::AcquireSRWLockExclusive(&m_lock);
	while (m_queuedCount == 0)
	{
		// Queued 된 패킷이 없는 경우
		// Lock 을 해제하고 Sleep 상태로 들어간다.
		if (::ReadAcquire(&m_running) == FALSE)
		{
			::ReleaseSRWLockExclusive(&m_lock);
			return nullptr;
		}

		::SleepConditionVariableSRW(&m_cv, &m_lock, INFINITE, 0);
	}

	// EnqueuePacket 내부에서 WakeConditionVariable 로 Sleep 을 깨운 경우
	// 현재 처리 중인 패킷이 있다면 종료한다.
	if (::ReadAcquire(&m_hasHeldPacket) == TRUE)
	{
		::ReleaseSRWLockExclusive(&m_lock);
		return nullptr;
	}

	// readPos 에 위치한 패킷 데이터를 반환한다.
	const size_t heldIndex = m_readPos;
	m_states[heldIndex] = SLOT_HELD;

	// m_heldPos 를 업데이트 하여 ReleaseReadPacket 에서 사용하도록 한다.
	// 외부에 Buffer Index 를 알리지 않기 위함.
	m_heldPos = heldIndex;
	::InterlockedExchange(&m_hasHeldPacket, TRUE);
	--m_queuedCount;

	// 다음 Read Pos 계산
	if (m_queuedCount > 0)
	{
		// 처리해야할 데이터가 남아 있는 경우
		// Queued 되어 있는 Slot 을 찾는다.
		m_readPos = FindNextSlotWithState(m_states, m_packetCount, WrapRingIndex(heldIndex + 1, m_packetCount), SLOT_QUEUED);
	}
	else
	{
		// 처리해야할 데이터가 없는 경우
		// 단순하게 Read Pos 인덱스만 증가
		m_readPos = WrapRingIndex(heldIndex + 1, m_packetCount);
	}

	DecodePacketItem* item = &m_items[heldIndex];
	::ReleaseSRWLockExclusive(&m_lock);

	return item;
}

void DecodePacketQueue::ReleaseReadPacket()
{
	// AcquireReadPacket 로 획득한 패킷 데이터를 Release 하는 함수
	// AcquireReadPacket 에서 설정한 m_heldPos 을 사용한다.

	if (!m_items || !m_states)
	{
		return;
	}

	::AcquireSRWLockExclusive(&m_lock);

	// 실제로 패킷을 Acquire 했는지 체크 후
	// 슬롯 상태를 초기화 해준다.
	if (::ReadAcquire(&m_hasHeldPacket) == TRUE)
	{
		m_states[m_heldPos] = SLOT_FREE;
		m_items[m_heldPos].size = 0;
		m_items[m_heldPos].timestamp = 0;
		::InterlockedExchange(&m_hasHeldPacket, FALSE);
		::InterlockedIncrement(&m_dequeuedCount);
	}
	::ReleaseSRWLockExclusive(&m_lock);
}

void DecodePacketQueue::Shutdown()
{
	// Running State 를 변경 후 Sleep 중인 스레드를 깨워 스레드가 정상 종료 되도록 함

	::InterlockedExchange(&m_running, FALSE);

	::AcquireSRWLockExclusive(&m_lock);
	::WakeAllConditionVariable(&m_cv);
	::ReleaseSRWLockExclusive(&m_lock);
}

bool DecodePacketQueue::IsRunning() const
{
	return ::ReadAcquire(&m_running) != FALSE;
}

uint32_t DecodePacketQueue::GetDequeuedCount() const
{
	return static_cast<uint32_t>(::ReadAcquire(&m_dequeuedCount));
}

uint64_t DecodePacketQueue::GetDropCount() const
{
	return static_cast<uint64_t>(
		::ReadAcquire64(&m_dropCount));
}

size_t DecodePacketQueue::GetBufferSize() const
{
	return m_bufferSize;
}
