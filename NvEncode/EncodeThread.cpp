#include "pch.h"
#include "EncodeThread.h"

#include "D3D11NvEncoder_Impl.h"
#include "EncodeFrameQueue.h"

#include <stdio.h> // for printf_s

EncodeThread::EncodeThread()
	: Core::Concurrency::ThreadBase(L"EncodeThread")
{
}

EncodeThread::~EncodeThread()
{
	Shutdown();
}

bool EncodeThread::Initialize(EncodeFrameQueue* queue, D3D11NvEncoder_Impl* encoder)
{
	if (!queue || !encoder)
		return false;

	// 이 스레드는 SubmitFrame 만 호출하고 출력 회수는 하지 않는다.
	// 출력을 드레인하는 것은 엔코더 내부의 EncodeCompletionThread 이고,
	// 그 스레드는 async 파이프라인일 때만 생성된다.
	//
	// 동기 모드 엔코더를 붙이면 아무도 출력을 회수하지 않아
	// pending 이 encodeSlotCount 까지 차오른 뒤 CanSubmitFrame() 이
	// 영구히 false 가 되고, 파이프라인이 조용히 정지한다.
	// (동기 모드에서는 m_allSlotsFreeEvent 도 생성되지 않아
	//  WaitForPendingFrames() 가 항상 false 를 반환한다.)
	//
	// 동기 인코딩은 호출자가 StageFrame + EncodeSync 를
	// 직접 돌려야 한다. 이 스레드를 쓰지 않는다.
	//
	// 주의: 엔코더가 Initialize 된 뒤에 호출해야 한다.
	// 초기화 전에는 이 값이 기본값(true)이라 검사가 무의미하다.
	if (!encoder->IsAsyncPipelineEnabled())
	{
		printf_s("[NVENC ERROR] StartEncodeThread requires an encoder with the async pipeline"
			" enabled. A sync-mode encoder would stall because nothing drains its output."
			" Use StageFrame + EncodeSync on the calling thread instead.\n");
		return false;
	}

	Shutdown();

	m_inputQueue = queue;
	m_encoder = encoder;

	if (!Start())
	{
		m_inputQueue = nullptr;
		m_encoder = nullptr;
		return false;
	}

	return true;
}

void EncodeThread::Shutdown()
{
	// 유입 큐부터 닫아 대기 중인 리더를 깨운다
	if (m_inputQueue)
	{
		m_inputQueue->Shutdown();
	}

	// 스레드 종료
	Stop();

	if (m_encoder)
	{
		// 아직 pending 인 프레임이 남아 있으면 그 패킷은 앱에 도달하지 못한다.
		// 콜백을 여기서 떼지는 않는다 — 그건 앱이 엔코더에 등록한 것이고
		// 이 스레드가 관여할 소유물이 아니다.
		if (!m_encoder->WaitForPendingFrames(20'000U))
		{
			NvEncStats stats = {};
			m_encoder->GetStats(stats);
			printf_s("[NVENC WARNING] Shutting down with %u frames still pending."
				" (faulted=%d) Their packets are dropped.\n",
				stats.pendingFrames, stats.faulted ? 1 : 0);
		}
	}

	m_inputQueue = nullptr;
	m_encoder = nullptr;
}

void EncodeThread::SetKeyFrameRequestCallback(KeyFrameRequestCallback callback, void* userData)
{
	::AcquireSRWLockExclusive(&m_callbackLock);
	m_keyFrameRequestCallback = callback;
	m_keyFrameRequestCallbackUserData = userData;
	::ReleaseSRWLockExclusive(&m_callbackLock);
}

void EncodeThread::FillStats(NvEncStats& stats) const
{
	stats.droppedNoEncoderSlot = static_cast<uint64_t>(
		::ReadAcquire64(&m_droppedNoEncoderSlotCount));
	stats.droppedPrepareFailed = static_cast<uint64_t>(
		::ReadAcquire64(&m_droppedPrepareFailedCount));
	stats.droppedSubmitFailed = static_cast<uint64_t>(
		::ReadAcquire64(&m_droppedSubmitFailedCount));
}

bool EncodeThread::QueryKeyFrameRequest()
{
	bool requested = false;

	::AcquireSRWLockShared(&m_callbackLock);
	if (m_keyFrameRequestCallback)
		requested = m_keyFrameRequestCallback(m_keyFrameRequestCallbackUserData);
	::ReleaseSRWLockShared(&m_callbackLock);

	return requested;
}

void EncodeThread::Run()
{
	while (!IsStopRequested())
	{
		EncodeFrameQueue::EncodeFrameItem* frameItem = m_inputQueue->AcquireReadFrame();
		if (!frameItem)
		{
			// 큐가 닫혔으면 정상 종료다.
			// 그렇지 않다면 HELD 프레임이 남아있다는 뜻이고, 이는 프로그래밍 오류다.
			if (m_inputQueue->IsRunning())
			{
				printf_s("[NVENC ERROR] Encode thread stopping: the queue still holds a frame."
					" ReleaseReadFrame was not called.\n");
			}
			break;
		}

		const uint64_t frameId = frameItem->inputFrame.frameId;
		bool forceKeyFrame = frameItem->forceKeyFrame;
		if (QueryKeyFrameRequest())
		{
			forceKeyFrame = true;
		}

		if (forceKeyFrame)
		{
			// 인코더 슬롯이 없어 이 프레임을 버리게 되더라도 요청은 남겨 둔다.
			// 실제 제출 시점에 EncodePicture 가 소비한다.
			m_encoder->RequestKeyFrame();
		}

		if (!m_encoder->CanSubmitFrame())
		{
			// 인코더 슬롯이 없어 이 프레임을 버린다.
			// 큐의 dropCount 에는 잡히지 않으므로 여기서 센다.
			::InterlockedIncrement64(&m_droppedNoEncoderSlotCount);
			m_inputQueue->ReleaseReadFrame();
			continue;
		}

		bool prepareSucceeded = false;

		// 입력이 두 가지다.
		//
		//   texture 가 있으면 우리 디바이스의 텍스처다(같은 디바이스 구성).
		//   없으면 sourceSlotId 가 공유 풀의 슬롯 번호다.
		//
		// 후자가 캡처와 인코더가 서로 다른 D3D11 디바이스를 쓰는 구성이다.
		// 그때 texture 포인터는 남의 디바이스 것이라 여기서 쓸 수 없고,
		// 실제로 앱이 nullptr 로 채워 보낸다.
		if (frameItem->inputFrame.texture)
		{
			prepareSucceeded = m_encoder->StageFrame(frameItem->inputFrame.texture);
		}
		else if (frameItem->inputFrame.sourceSlotId >= 0)
		{
			prepareSucceeded = m_encoder->StageFrameFromSharedSlot(
				static_cast<uint32_t>(frameItem->inputFrame.sourceSlotId));
		}

		m_inputQueue->ReleaseReadFrame();

		if (!prepareSucceeded)
		{
			::InterlockedIncrement64(&m_droppedPrepareFailedCount);
			continue;
		}

		if (!m_encoder->SubmitFrame(frameId))
		{
			::InterlockedIncrement64(&m_droppedSubmitFailedCount);
			continue;
		}
	}
}
