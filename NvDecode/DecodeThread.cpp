#include "pch.h"
#include "DecodeThread.h"

#include "D3D11NvDecoder_Impl.h"
#include "DecodeFrameQueue.h"

#include <stdio.h> // for printf_s

DecodeThread::DecodeThread()
	: Core::Concurrency::ThreadBase(L"DecodeThread")
{
}

DecodeThread::~DecodeThread()
{
	Shutdown();
}

bool DecodeThread::Initialize(DecodeFrameQueue* queue, D3D11NvDecoder_Impl* decoder)
{
	if (!queue || !decoder)
		return false;

	if (!queue->IsValid())
	{
		printf_s("[NVDEC ERROR] StartDecodeThread got a queue that failed to allocate.\n");
		return false;
	}

	Shutdown();

	m_decodeFrameQueue = queue;
	m_decoder = decoder;

	if (!Start())
	{
		m_decodeFrameQueue = nullptr;
		m_decoder = nullptr;
		return false;
	}

	return true;
}

void DecodeThread::Shutdown()
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

void DecodeThread::SetFrameCallback(FrameCallback callback, void* userData)
{
	// 디코드 스레드가 읽는 값이므로 잠금이 필요하다.
	::AcquireSRWLockExclusive(&m_callbackLock);
	m_frameCallback = callback;
	m_frameCallbackUserData = userData;
	::ReleaseSRWLockExclusive(&m_callbackLock);
}

void DecodeThread::FillStats(NvDecStats& stats) const
{
	stats.packetsFailed = static_cast<uint64_t>(::ReadAcquire64(&m_packetsFailedCount));
}

void DecodeThread::DispatchFrame(const D3D11NvDecoder::Frame& frame)
{
	::AcquireSRWLockShared(&m_callbackLock);
	if (m_frameCallback)
	{
		m_frameCallback(frame, m_frameCallbackUserData);
	}
	::ReleaseSRWLockShared(&m_callbackLock);
}

void DecodeThread::Run()
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
			if (m_decoder->Parse(frameItem->data, static_cast<uint32_t>(frameItem->size),
				frameItem->timestamp, true, false, false))
			{
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
