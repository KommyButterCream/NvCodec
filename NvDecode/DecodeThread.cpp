#include "pch.h"
#include "DecodeThread.h"

#include "D3D11NvDecoder_Impl.h"
#include "DecodePacketQueue.h"

#include <stdio.h> // for printf_s

DecodeThread::DecodeThread()
	: Core::Concurrency::ThreadBase(L"DecodeThread")
{
}

DecodeThread::~DecodeThread()
{
	Shutdown();
}

bool DecodeThread::Initialize(DecodePacketQueue* queue, D3D11NvDecoder_Impl* decoder)
{
	if (!queue || !decoder)
		return false;

	Shutdown();

	m_inputQueue = queue;
	m_decoder = decoder;

	if (!Start())
	{
		m_inputQueue = nullptr;
		m_decoder = nullptr;
		return false;
	}

	return true;
}

void DecodeThread::Shutdown()
{
	// 유입 큐부터 닫아 대기 중인 리더를 깨운다
	if (m_inputQueue)
	{
		m_inputQueue->Shutdown();
	}

	// 스레드 종료
	Stop();

	m_inputQueue = nullptr;
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
		// 큐에서 패킷 하나 획득
		DecodePacketQueue::DecodePacketItem* packetItem = m_inputQueue->AcquireReadPacket();
		if (!packetItem)
		{
			// 큐가 닫혔으면 정상 종료다.
			// 그렇지 않다면 HELD 패킷이 남아있다는 뜻이고, 이는 프로그래밍 오류다.
			if (m_inputQueue->IsRunning())
			{
				printf_s("[NVDEC ERROR] Decode thread stopping: the queue still holds a frame."
					" ReleaseReadPacket was not called.\n");
			}
			break;
		}

		if (packetItem->size > 0)
		{
			// 디코딩 요청
			if (m_decoder->Parse(packetItem->data, static_cast<uint32_t>(packetItem->size),
				packetItem->timestamp, true, false, false))
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

		// 꺼낸 패킷 슬롯 반납
		m_inputQueue->ReleaseReadPacket();
	}
}
