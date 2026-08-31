#include "pch.h"
#include "EncodeThread_Impl.h"

#include "D3D11NvEncoder.h"
#include "EncodeFrameQueue.h"

#include <stdio.h> // for printf_s

EncodeThread_Impl::EncodeThread_Impl()
	: Core::Concurrency::ThreadBase(L"EncodeThread")
{
}

EncodeThread_Impl::~EncodeThread_Impl()
{
	Shutdown();
}

bool EncodeThread_Impl::Initialize(EncodeFrameQueue* queue, D3D11NvEncoder* encoder)
{
	if (!queue || !encoder)
		return false;

	// 이 스레드는 SubmitFrame 만 호출하고 출력 회수는 하지 않는다.
	// 출력을 드레인하는 것은 엔코더 내부의 EncodeCompletionThread 이고,
	// 그 스레드는 async 파이프라인일 때만 생성된다.
	//
	// 동기 모드 엔코더를 붙이면 아무도 출력을 회수하지 않아
	// pending 이 encodeBufferCount 까지 차오른 뒤 CanSubmitFrame() 이
	// 영구히 false 가 되고, 파이프라인이 조용히 정지한다.
	// (동기 모드에서는 m_allSlotsFreeEvent 도 생성되지 않아
	//  WaitForPendingFrames() 가 항상 false 를 반환한다.)
	//
	// 동기 인코딩은 호출자가 PrepareFrameForEncode + DoEncode 를
	// 직접 돌려야 한다. 이 스레드를 쓰지 않는다.
	//
	// 주의: 엔코더가 Initialize 된 뒤에 호출해야 한다.
	// 초기화 전에는 이 값이 기본값(true)이라 검사가 무의미하다.
	if (!encoder->IsAsyncPipelineEnabled())
	{
		printf_s("[NVENC ERROR] EncodeThread requires an encoder with the async pipeline enabled."
			" A sync-mode encoder would stall because nothing drains its output."
			" Use PrepareFrameForEncode + DoEncode on the calling thread instead.\n");
		return false;
	}

	Shutdown();

	// 외부로부터 EncodeFrameQueue 와 NvEncoder 입력 및 설정
	m_encodeFrameQueue = queue;
	m_encoder = encoder;
	m_encoder->SetEncodedPacketCallback(EncodedPacketCallback, this);

	// 엔코드 스레드 시작
	if (!Start())
	{
		m_encoder->SetEncodedPacketCallback(nullptr, nullptr);
		m_encodeFrameQueue = nullptr;
		m_encoder = nullptr;
		return false;
	}

	return true;
}

void EncodeThread_Impl::Shutdown()
{
	// 엔코딩 프레임 큐 부터 종료 알림
	if (m_encodeFrameQueue)
	{
		m_encodeFrameQueue->Shutdown();
	}

	// 스레드 종료
	Stop();

	if (m_encoder)
	{
		// 콜백을 먼저 떼면 아직 pending 인 프레임의 결과가 조용히 사라진다.
		// 반드시 드레인이 끝난 것을 확인한 뒤에 떼어야 한다.
		if (!m_encoder->WaitForPendingFrames())
		{
			NvEncStats stats = {};
			m_encoder->GetStats(stats);
			printf_s("[NVENC WARNING] Shutting down with %u frames still pending."
				" (faulted=%d) Their packets are dropped.\n",
				stats.pendingFrames, stats.faulted ? 1 : 0);
		}

		m_encoder->SetEncodedPacketCallback(nullptr, nullptr);
	}

	m_encodeFrameQueue = nullptr;
	m_encoder = nullptr;
}

void EncodeThread_Impl::SetEncodedFrameCallback(EncodeThread::EncodedFrameCallback callback, void* userData)
{
	// 엔코딩 끝난 후 호출될 콜백 함수 설정
	// 완료 스레드가 읽는 값이므로 잠금이 필요하다.
	::AcquireSRWLockExclusive(&m_callbackLock);
	m_encodedFrameCallback = callback;
	m_encodedFrameCallbackUserData = userData;
	::ReleaseSRWLockExclusive(&m_callbackLock);
}

void EncodeThread_Impl::SetKeyFrameRequestCallback(EncodeThread::KeyFrameRequestCallback callback, void* userData)
{
	// 키프레임을 포함하여 엔코딩을 할 것인지 확인하는 콜백 함수 설정
	::AcquireSRWLockExclusive(&m_callbackLock);
	m_keyFrameRequestCallback = callback;
	m_keyFrameRequestCallbackUserData = userData;
	::ReleaseSRWLockExclusive(&m_callbackLock);
}

void EncodeThread_Impl::GetStats(EncodeThread::Stats& stats) const
{
	stats.framesSubmitted = static_cast<uint64_t>(
		::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_submittedFrameCount), 0, 0));
	stats.framesDroppedNoEncoderSlot = static_cast<uint64_t>(
		::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_droppedNoEncoderSlotCount), 0, 0));
	stats.framesDroppedPrepareFailed = static_cast<uint64_t>(
		::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_droppedPrepareFailedCount), 0, 0));
	stats.framesDroppedSubmitFailed = static_cast<uint64_t>(
		::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_droppedSubmitFailedCount), 0, 0));
}

bool EncodeThread_Impl::QueryKeyFrameRequest()
{
	bool requested = false;

	::AcquireSRWLockShared(&m_callbackLock);
	if (m_keyFrameRequestCallback)
		requested = m_keyFrameRequestCallback(m_keyFrameRequestCallbackUserData);
	::ReleaseSRWLockShared(&m_callbackLock);

	return requested;
}

void EncodeThread_Impl::Run()
{
	while (!IsStopRequested())
	{
		EncodeFrameQueue::EncodeFrameItem* frameItem = m_encodeFrameQueue->AcquireReadFrame();
		if (!frameItem)
		{
			// 큐가 닫혔으면 정상 종료다.
			// 그렇지 않다면 HELD 프레임이 남아있다는 뜻이고, 이는 프로그래밍 오류다.
			// 두 경우를 구분해 로그를 남긴다(예전에는 둘 다 조용히 종료됐다).
			if (m_encodeFrameQueue->IsRunning())
			{
				printf_s("[NVENC ERROR] Encode thread stopping: the queue still holds a frame."
					" ReleaseReadFrame was not called.\n");
			}
			break;
		}

		const uint64_t frameId = frameItem->frameHandle.frameId;
		bool forceKeyFrame = frameItem->forceKeyFrame;
		if (QueryKeyFrameRequest())
		{
			forceKeyFrame = true;
		}

		if (forceKeyFrame)
		{
			// Keep the request pending even when this frame is dropped because all
			// encoder slots are busy. EncodePicture consumes it on actual submission.
			m_encoder->RequestKeyFrame();
		}

		if (!m_encoder->CanSubmitFrame())
		{
			// 인코더 슬롯이 없어 이 프레임을 버린다.
			// 큐의 dropCount 에는 잡히지 않으므로 여기서 센다.
			::InterlockedIncrement64(&m_droppedNoEncoderSlotCount);
			m_encodeFrameQueue->ReleaseReadFrame();
			continue;
		}

		bool prepareSucceeded = false;
		if (frameItem->frameHandle.texture)
		{
			prepareSucceeded = m_encoder->PrepareFrameForEncode(frameItem->frameHandle.texture);
		}

		m_encodeFrameQueue->ReleaseReadFrame();

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

		::InterlockedIncrement64(&m_submittedFrameCount);
	}
}

void EncodeThread_Impl::EncodedPacketCallback(const NvEncPacket& packet, void* userData)
{
	EncodeThread_Impl* self = static_cast<EncodeThread_Impl*>(userData);
	if (self)
		self->DispatchEncodedFrame(packet);
}

void EncodeThread_Impl::DispatchEncodedFrame(const NvEncPacket& packet)
{
	if (!packet.data || packet.size == 0)
		return;

	EncodeThread::EncodedFrame encodedFrame = {};
	encodedFrame.data = packet.data;
	encodedFrame.size = packet.size;
	encodedFrame.frameId = packet.frameId;
	encodedFrame.timestamp = packet.timestamp;
	encodedFrame.frameType = packet.frameType;
	encodedFrame.isKeyFrame = packet.isKeyFrame;

	// 콜백 포인터를 잠금 아래에서 읽는다. 콜백 자체도 잠금 안에서 호출해
	// 호출 중에 userData 가 교체되지 않도록 한다(엔코더 레벨과 같은 방식).
	::AcquireSRWLockShared(&m_callbackLock);
	if (m_encodedFrameCallback)
		m_encodedFrameCallback(encodedFrame, m_encodedFrameCallbackUserData);
	::ReleaseSRWLockShared(&m_callbackLock);
}
