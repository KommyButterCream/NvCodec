#include "pch.h"
#include "DecodeFrameQueue.h"

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

	inline size_t FindNextSlotWithState(const DecodeFrameQueue::SlotState* states, size_t bufferCount, size_t start, DecodeFrameQueue::SlotState state)
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

DecodeFrameQueue::DecodeFrameQueue(size_t bufferSize, size_t bufferCount)
	: m_bufferCount(bufferCount)
{
	if (bufferSize == 0 || bufferCount == 0)
	{
		return;
	}

	// 버퍼 수량이 2 의 n 승일 것임을 보장 해야 한다.
	if (!IsPowerOfTwo(m_bufferCount))
	{
		assert(false && "bufferCount must be a power of two");
		return;
	}

	// 64 바이트 얼라인 된 버퍼 1개의 크기를 계산
	m_bufferSize = AlignUp(bufferSize, 64);

	// 필요로되는 전체 메모리 크기만큼 선형적 메모리를 할당
	const size_t totalSize = m_bufferSize * m_bufferCount;

	// 디코딩 Raw 데이터 저장을 위한 1D Linear 버퍼 할당
	m_buffers = static_cast<uint8_t*>(_aligned_malloc(totalSize, 64));
	if (!m_buffers)
	{
		m_bufferCount = 0;
		m_bufferSize = 0;
		return;
	}

	// 디코딩 데이터를 저장할 구조체 버퍼 할당
	m_items = new DecodeFrameItem[m_bufferCount]();
	if (!m_items)
	{
		_aligned_free(m_buffers);
		m_buffers = nullptr;
		m_bufferCount = 0;
		m_bufferSize = 0;
		return;
	}

	// 동일 수량만큼의 Slot 상태 저장하는 버퍼 할당
	m_states = new SlotState[m_bufferCount]();
	if (!m_states)
	{
		delete[] m_items;
		m_items = nullptr;
		_aligned_free(m_buffers);
		m_buffers = nullptr;
		m_bufferCount = 0;
		m_bufferSize = 0;
		return;
	}

	// 기본값 초기화
	for (size_t i = 0; i < m_bufferCount; ++i)
	{
		m_items[i].data = m_buffers + (m_bufferSize * i);
		m_items[i].size = 0;
		m_items[i].frameId = 0;
		m_items[i].timestamp = 0;
		m_items[i].frameType = 0;
		m_states[i] = SLOT_FREE;
	}
}

DecodeFrameQueue::~DecodeFrameQueue()
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

bool DecodeFrameQueue::EnqueueFrame(const InputFrameHandle& frameHandle)
{
	// 프레임 핸들을 디코드 프레임 큐 버퍼에 저장한다.
	// 디코드 프레임 큐는 고정 크기 링 버퍼 형태이다.
	if (!m_items || !m_buffers || !m_states || (!frameHandle.data && frameHandle.size > 0) || frameHandle.size > m_bufferSize)
	{
		return false;
	}

	// DecodeFrameQueue 사용 중이 아니라면 종료
	if (::ReadAcquire(&m_running) == FALSE)
	{
		return false;
	}

	// m_items, m_states 접근을 위한 Lock
	::AcquireSRWLockExclusive(&m_lock);

	const bool wasEmpty = (m_queuedCount == 0);
	const bool hasHeldFrame = (::ReadAcquire(&m_hasHeldFrame) == TRUE);
	const size_t occupiedCount = m_queuedCount + (hasHeldFrame ? 1 : 0);

	// 프레임 핸들을 저장하기 위한 비어 있는 슬롯을 찾는다.
	if (occupiedCount >= m_bufferCount)
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
		m_items[droppedIndex].frameId = 0;
		m_items[droppedIndex].timestamp = 0;
		m_items[droppedIndex].frameType = 0;
		m_readPos = FindNextSlotWithState(m_states, m_bufferCount, WrapRingIndex(droppedIndex + 1, m_bufferCount), SLOT_QUEUED);

		--m_queuedCount;
		::InterlockedIncrement64(&m_dropCount);
		m_writePos = droppedIndex;
	}
	else
	{
		// 사용 가능한 슬롯이 있는 경우 FREE 상태인 슬롯 인덱스 반환
		const size_t freeIndex = FindNextSlotWithState(m_states, m_bufferCount, m_writePos, SLOT_FREE);
		if (freeIndex >= m_bufferCount)
		{
			::ReleaseSRWLockExclusive(&m_lock);
			return false;
		}

		m_writePos = freeIndex;
	}

	// 데이터 저장을 위한 슬롯 ID 계산이 끝났으므로 (m_writePos)
	// 해당 위치에 프레임 데이터를 저장한다.
	// 이때, Encode Raw Data 를 슬롯 버퍼로 Deep-Copy 복사하여 저장한다.
	DecodeFrameItem& item = m_items[m_writePos];
	item.size = frameHandle.size;
	item.frameId = frameHandle.frameId;
	item.timestamp = frameHandle.timestamp;
	item.frameType = frameHandle.frameType;
	if (frameHandle.size > 0)
	{
		memcpy(item.data, frameHandle.data, frameHandle.size);
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
	m_writePos = WrapRingIndex(m_writePos + 1, m_bufferCount);

	// 데이터 처리 중이 아니라면 스레드를 깨워 작업을 시키기 위해
	// Condition_Variable 를 깨운다.
	if (wasEmpty)
	{
		::WakeConditionVariable(&m_cv);
	}

	::ReleaseSRWLockExclusive(&m_lock);

	return true;
}

DecodeFrameQueue::DecodeFrameItem* DecodeFrameQueue::AcquireReadFrame()
{
	// 디코드 스레드 측에서 디코딩할 데이터를 획득하는 함수

	if (!m_items || !m_buffers || !m_states)
	{
		return nullptr;
	}

	::AcquireSRWLockExclusive(&m_lock);
	while (m_queuedCount == 0)
	{
		// Queued 된 프레임이 없는 경우
		// Lock 을 해제하고 Sleep 상태로 들어간다.
		if (::ReadAcquire(&m_running) == FALSE)
		{
			::ReleaseSRWLockExclusive(&m_lock);
			return nullptr;
		}

		::SleepConditionVariableSRW(&m_cv, &m_lock, INFINITE, 0);
	}

	// EnqueueFrame 내부에서 WakeConditionVariable 로 Sleep 을 깨운 경우
	// 현재 처리 중인 프레임이 있다면 종료한다.
	if (::ReadAcquire(&m_hasHeldFrame) == TRUE)
	{
		::ReleaseSRWLockExclusive(&m_lock);
		return nullptr;
	}

	// readPos 에 위치한 프레임 데이터를 반환한다.
	const size_t heldIndex = m_readPos;
	m_states[heldIndex] = SLOT_HELD;

	// m_heldPos 를 업데이트 하여 ReleaseReadFrame 에서 사용하도록 한다.
	// 외부에 Buffer Index 를 알리지 않기 위함.
	m_heldPos = heldIndex;
	::InterlockedExchange(&m_hasHeldFrame, TRUE);
	--m_queuedCount;

	// 다음 Read Pos 계산
	if (m_queuedCount > 0)
	{
		// 처리해야할 데이터가 남아 있는 경우
		// Queued 되어 있는 Slot 을 찾는다.
		m_readPos = FindNextSlotWithState(m_states, m_bufferCount, WrapRingIndex(heldIndex + 1, m_bufferCount), SLOT_QUEUED);
	}
	else
	{
		// 처리해야할 데이터가 없는 경우
		// 단순하게 Read Pos 인덱스만 증가
		m_readPos = WrapRingIndex(heldIndex + 1, m_bufferCount);
	}

	DecodeFrameItem* item = &m_items[heldIndex];
	::ReleaseSRWLockExclusive(&m_lock);

	return item;
}

void DecodeFrameQueue::ReleaseReadFrame()
{
	// AcquireReadFrame 로 획득한 프레임 데이터를 Release 하는 함수
	// AcquireReadFrame 에서 설정한 m_heldPos 을 사용한다.

	if (!m_items || !m_states)
	{
		return;
	}

	::AcquireSRWLockExclusive(&m_lock);

	// 실제로 프레임을 Acquire 했는지 체크 후
	// 슬롯 상태를 초기화 해준다.
	if (::ReadAcquire(&m_hasHeldFrame) == TRUE)
	{
		m_states[m_heldPos] = SLOT_FREE;
		m_items[m_heldPos].size = 0;
		m_items[m_heldPos].frameId = 0;
		m_items[m_heldPos].timestamp = 0;
		m_items[m_heldPos].frameType = 0;
		::InterlockedExchange(&m_hasHeldFrame, FALSE);
		::InterlockedIncrement(&m_processCount);
	}
	::ReleaseSRWLockExclusive(&m_lock);
}

void DecodeFrameQueue::Shutdown()
{
	// Running State 를 변경 후 Sleep 중인 스레드를 깨워 스레드가 정상 종료 되도록 함

	::InterlockedExchange(&m_running, FALSE);

	::AcquireSRWLockExclusive(&m_lock);
	::WakeAllConditionVariable(&m_cv);
	::ReleaseSRWLockExclusive(&m_lock);
}

bool DecodeFrameQueue::IsValid() const
{
	return m_items != nullptr && m_buffers != nullptr && m_states != nullptr && m_bufferCount > 0;
}

bool DecodeFrameQueue::IsRunning() const
{
	return ::ReadAcquire(&m_running) != FALSE;
}

int32_t DecodeFrameQueue::GetProcessCount() const
{
	return ::ReadAcquire(&m_processCount);
}

uint64_t DecodeFrameQueue::GetDropCount() const
{
	return static_cast<uint64_t>(
		::ReadAcquire64(&m_dropCount));
}

size_t DecodeFrameQueue::GetBufferSize() const
{
	return m_bufferSize;
}
