#include "pch.h"
#include "DecodeThread_Impl.h"

#include "DecodeFrameQueue.h"

#include <stdio.h> // for printf_s

DecodeThread_Impl::DecodeThread_Impl()
	: Core::Concurrency::ThreadBase(L"DecodeThread")
{
}

DecodeThread_Impl::~DecodeThread_Impl()
{
	Shutdown();
}

bool DecodeThread_Impl::Initialize(DecodeFrameQueue* queue, D3D11NvDecoder* decoder)
{
	if (!queue || !decoder)
		return false;

	if (!queue->IsValid())
	{
		printf_s("[NVDEC ERROR] DecodeThread got a queue that failed to allocate.\n");
		return false;
	}

	Shutdown();

	// 외부로부터 DecodeFrameQueue 와 NvDecoder 입력 및 설정
	m_decodeFrameQueue = queue;
	m_decoder = decoder;

	// 디코드 스레드 시작
	if (!Start())
	{
		m_decodeFrameQueue = nullptr;
		m_decoder = nullptr;
		return false;
	}

	return true;
}

void DecodeThread_Impl::Shutdown()
{
	// 디코딩 프레임 큐 부터 종료 알림
	if (m_decodeFrameQueue)
	{
		m_decodeFrameQueue->Shutdown();
	}

	// 스레드 종료
	Stop();

	m_decodeFrameQueue = nullptr;
	m_decoder = nullptr;
}

void DecodeThread_Impl::SetFrameCallback(FrameCallback callback, void* userData)
{
	// 디코딩이 끝난 후 호출될 콜백 함수 설정
	// 디코드 스레드가 읽는 값이므로 잠금이 필요하다.
	::AcquireSRWLockExclusive(&m_callbackLock);
	m_frameCallback = callback;
	m_frameCallbackUserData = userData;
	::ReleaseSRWLockExclusive(&m_callbackLock);
}

void DecodeThread_Impl::GetStats(DecodeThread::Stats& stats) const
{
	stats.packetsParsed = static_cast<uint64_t>(
		::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_packetsParsedCount), 0, 0));
	stats.packetsFailed = static_cast<uint64_t>(
		::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_packetsFailedCount), 0, 0));
	stats.framesDelivered = static_cast<uint64_t>(
		::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_framesDeliveredCount), 0, 0));
}

void DecodeThread_Impl::DispatchFrame(const D3D11NvDecoder::Frame& frame)
{
	::AcquireSRWLockShared(&m_callbackLock);
	if (m_frameCallback)
	{
		m_frameCallback(frame, m_frameCallbackUserData);
	}
	::ReleaseSRWLockShared(&m_callbackLock);
}

void DecodeThread_Impl::Run()
{
	while (!IsStopRequested())
	{
		// 디코딩 프레임 아이템 하나 획득
		DecodeFrameQueue::DecodeFrameItem* frameItem = m_decodeFrameQueue->AcquireReadFrame();
		if (!frameItem)
		{
			// 큐가 닫혔으면 정상 종료다.
			// 그렇지 않다면 HELD 프레임이 남아있다는 뜻이고, 이는 프로그래밍 오류다.
			if (m_decodeFrameQueue->IsRunning())
			{
				printf_s("[NVDEC ERROR] Decode thread stopping: the queue still holds a frame."
					" ReleaseReadFrame was not called.\n");
			}
			break;
		}

		if (frameItem->size > 0)
		{
			// 디코딩 요청
			if (m_decoder->Parse(frameItem->data, static_cast<uint32_t>(frameItem->size), frameItem->timestamp))
			{
				::InterlockedIncrement64(&m_packetsParsedCount);

				// 디코딩 결과 프레임을 꺼내 콜백에 넘긴다.
				//
				// AcquireFrame 으로 받은 프레임은 반드시 ReleaseFrame 으로 돌려줘야
				// 그 슬롯이 재사용된다. 콜백이 반환하면 다 쓴 것으로 보고 반납한다.
				// 콜백 밖으로 텍스처를 들고 나가려면 앱이 직접 AcquireFrame /
				// ReleaseFrame 을 쓰고 이 스레드를 쓰지 않아야 한다.
				while (D3D11NvDecoder::Frame* frame = m_decoder->AcquireFrame())
				{
					DispatchFrame(*frame);
					m_decoder->ReleaseFrame(frame);
					::InterlockedIncrement64(&m_framesDeliveredCount);
				}
			}
			else
			{
				::InterlockedIncrement64(&m_packetsFailedCount);
			}
		}

		// 디코딩 프레임 아이템 반환
		m_decodeFrameQueue->ReleaseReadFrame();
	}
}
