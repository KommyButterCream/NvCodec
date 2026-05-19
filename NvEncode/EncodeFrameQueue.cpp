#include "pch.h"
#include "EncodeFrameQueue.h"

#include <new> // for std::nothrow

EncodeFrameQueue::~EncodeFrameQueue()
{
	Shutdown();

	if (m_items)
	{
		for (uint32_t index = 0; index < m_frameCount; ++index)
		{
			ReleaseFrameHandle(m_items[index].frameHandle);
		}

		delete[] m_items;
		m_items = nullptr;
	}

	if (m_states)
	{
		delete[] m_states;
		m_states = nullptr;
	}

	m_frameCount = 0;
}

bool EncodeFrameQueue::Initialize(uint32_t frameCount, ReleaseFrameCallback releaseCallback, void* userData)
{
	Shutdown();

	// 버퍼 수량이 2 의 n 승일 것임을 보장 해야 한다.
	if (frameCount == 0 || !IsPowerOfTwo(frameCount) || !releaseCallback)
		return false;

	if (m_items || m_states)
		return false;

	// 엔코딩 데이터를 저장할 구조체 버퍼 할당
	m_items = new (std::nothrow) EncodeFrameItem[frameCount]();
	if (!m_items)
		return false;

	// 동일 수량만큼의 Slot 상태 저장하는 버퍼 할당
	m_states = new (std::nothrow) SlotState[frameCount]();
	if (!m_states)
	{
		delete[] m_items;
		m_items = nullptr;
		return false;
	}

	// 상태 초기화
	m_frameCount = frameCount;
	m_releaseCallback = releaseCallback;
	m_releaseCallbackUserData = userData;
	m_writePos = 0;
	m_readPos = 0;
	m_queuedCount = 0;
	m_heldPos = 0;

	::InterlockedExchange(&m_hasHeldFrame, FALSE);
	::InterlockedExchange(&m_running, TRUE);
	::InterlockedExchange(&m_dropCount, 0);
	::InterlockedExchange(&m_processCount, 0);

	for (uint32_t index = 0; index < m_frameCount; ++index)
	{
		m_states[index] = SLOT_FREE;
		m_items[index] = EncodeFrameItem();
	}

	return true;
}

void EncodeFrameQueue::Shutdown()
{
	// Running State 를 변경 후 Sleep 중인 스레드를 깨워 스레드가 정상 종료 되도록 함
	::InterlockedExchange(&m_running, FALSE);

	::AcquireSRWLockExclusive(&m_lock);
	::WakeAllConditionVariable(&m_cv);
	::ReleaseSRWLockExclusive(&m_lock);
}

bool EncodeFrameQueue::EnqueueLatest(const InputFrameHandle& frameHandle, bool forceKeyFrame)
{
	// 외부에서 받아온 FrameHandle 을 참조하여 사용만 하고
	// ReleaseFrameHandle 로 반환 해주어야 한다.

	if (!m_items || !m_states || !frameHandle.texture)
		return false;

	if (::InterlockedCompareExchange(&m_running, TRUE, TRUE) == FALSE)
		return false;

	::AcquireSRWLockExclusive(&m_lock);

	// Queued 되어 있는 슬롯을 FREE 상태로 변경하여 Drop
	DropQueuedFrames_NoLock();

	// 프레임 핸들을 저장하기 위한 비어 있는 슬롯을 찾는다.
	const uint32_t freeIndex = FindNextSlotWithState(m_writePos, SLOT_FREE);
	if (freeIndex >= m_frameCount)
	{
		::ReleaseSRWLockExclusive(&m_lock);
		return false;
	}

	// 해당 위치에 프레임 데이터를 저장하고
	// 해당 슬롯이 Queued 되어있음을 상태 변경
	m_writePos = freeIndex;
	m_items[m_writePos].frameHandle = frameHandle;
	m_items[m_writePos].forceKeyFrame = forceKeyFrame;
	m_states[m_writePos] = SLOT_QUEUED;
	m_readPos = m_writePos;

	// Queued Item 수량을 증가시키고
	// 다음 Write Pos 를 업데이트 한다.
	m_queuedCount = 1;
	m_writePos = WrapIndex(m_writePos + 1);

	// 스레드를 깨워 작업을 시키기 위해
	// Condition_Variable 를 깨운다.
	::WakeConditionVariable(&m_cv);
	::ReleaseSRWLockExclusive(&m_lock);

	return true;
}

EncodeFrameQueue::EncodeFrameItem* EncodeFrameQueue::AcquireReadFrame()
{
	if (!m_items || !m_states)
		return nullptr;

	::AcquireSRWLockExclusive(&m_lock);

	while (m_queuedCount == 0)
	{
		// Queued 된 프레임이 없는 경우
		// Lock 을 해제하고 Sleep 상태로 들어간다.
		if (::InterlockedCompareExchange(&m_running, TRUE, TRUE) == FALSE)
		{
			::ReleaseSRWLockExclusive(&m_lock);
			return nullptr;
		}

		::SleepConditionVariableSRW(&m_cv, &m_lock, INFINITE, 0);
	}

	// EnqueueFrame 내부에서 WakeConditionVariable 로 Sleep 을 깨운 경우
	// 현재 처리 중인 프레임이 있다면 종료한다.
	if (::InterlockedCompareExchange(&m_hasHeldFrame, TRUE, TRUE) == TRUE)
	{
		::ReleaseSRWLockExclusive(&m_lock);
		return nullptr;
	}

	// readPos 에 위치한 프레임 데이터를 반환한다.
	const uint32_t heldIndex = m_readPos;
	m_states[heldIndex] = SLOT_HELD;

	// m_heldPos 를 업데이트 하여 ReleaseReadFrame 에서 사용하도록 한다.
	// 외부에 Buffer Index 를 알리지 않기 위함.
	m_heldPos = heldIndex;
	--m_queuedCount;
	::InterlockedExchange(&m_hasHeldFrame, TRUE);

	// 다음 Read Pos 계산
	if (m_queuedCount > 0)
	{
		// 처리해야할 데이터가 남아 있는 경우
		// Queued 되어 있는 Slot 을 찾는다.
		m_readPos = FindNextSlotWithState(WrapIndex(heldIndex + 1), SLOT_QUEUED);
	}
	else
	{
		// 처리해야할 데이터가 없는 경우
		// 단순하게 Read Pos 인덱스만 증가
		m_readPos = WrapIndex(heldIndex + 1);
	}

	EncodeFrameItem* item = &m_items[heldIndex];
	::ReleaseSRWLockExclusive(&m_lock);

	return item;
}

void EncodeFrameQueue::ReleaseReadFrame()
{
	// AcquireReadFrame 로 획득한 프레임 데이터를 Release 하는 함수
	// AcquireReadFrame 에서 설정한 m_heldPos 을 사용한다.
	if (!m_items || !m_states)
		return;

	::AcquireSRWLockExclusive(&m_lock);

	// 실제로 프레임을 Acquire 했는지 체크 후
	// 슬롯 상태를 초기화 해준다.
	if (::InterlockedCompareExchange(&m_hasHeldFrame, TRUE, TRUE) == TRUE)
	{
		ReleaseFrameHandle(m_items[m_heldPos].frameHandle);
		m_items[m_heldPos] = EncodeFrameItem();
		m_states[m_heldPos] = SLOT_FREE;
		::InterlockedExchange(&m_hasHeldFrame, FALSE);
		::InterlockedIncrement(&m_processCount);
	}

	::ReleaseSRWLockExclusive(&m_lock);
}

uint32_t EncodeFrameQueue::GetDropCount() const
{
	return static_cast<uint32_t>(::InterlockedCompareExchange(const_cast<volatile LONG*>(&m_dropCount), 0, 0));
}

uint32_t EncodeFrameQueue::GetProcessCount() const
{
	return static_cast<uint32_t>(::InterlockedCompareExchange(const_cast<volatile LONG*>(&m_processCount), 0, 0));
}

void EncodeFrameQueue::ReleaseFrameHandle(InputFrameHandle& frameHandle)
{
	// 외부에서 받아온 FrameHandle 을 반환 해주기 위한 Callback 호출

	if (!frameHandle.texture)
		return;

	if (m_releaseCallback)
	{
		m_releaseCallback(frameHandle, m_releaseCallbackUserData);
	}
}

void EncodeFrameQueue::DropQueuedFrames_NoLock()
{
	// Queued 되어 있는 Slot 을 찾아 FREE 상태로 변경시켜
	// 대기 중인 프레임을 Drop 시킨다.
	for (uint32_t index = 0; index < m_frameCount; ++index)
	{
		if (m_states[index] != SLOT_QUEUED)
			continue;

		ReleaseFrameHandle(m_items[index].frameHandle);

		m_items[index] = EncodeFrameItem();
		m_states[index] = SLOT_FREE;
		::InterlockedIncrement(&m_dropCount);
	}

	m_queuedCount = 0;
}

uint32_t EncodeFrameQueue::FindNextSlotWithState(uint32_t start, SlotState state) const
{
	// Start Index 부터 시작하여 입력받은 State 와 동일한 Slot Index 를 반환.
	if (!m_states || m_frameCount == 0)
		return m_frameCount;

	for (uint32_t offset = 0; offset < m_frameCount; ++offset)
	{
		const uint32_t index = WrapIndex(start + offset);
		if (m_states[index] == state)
			return index;
	}

	return m_frameCount;
}

uint32_t EncodeFrameQueue::WrapIndex(uint32_t index) const
{
	// 버퍼 수량이 2의 n 승을 보장하므로
	// Bit And 연산을 통해 Warp-Around.
	// '%' 연산보다 속도가 빠르다.
	return index & (m_frameCount - 1);
}

bool EncodeFrameQueue::IsPowerOfTwo(uint32_t value) const
{
	// 입력받은 수가 2 의 n 승 인지 확인하여 상태 반환
	return value != 0 && (value & (value - 1)) == 0;
}
