#include "pch.h"
#include "DecodeThread_Impl.h"

#include "DecodeFrameQueue.h"

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
	m_frameCallback = callback;
	m_frameCallbackUserData = userData;
}

void DecodeThread_Impl::Run()
{
	// 스레드
	while (!IsStopRequested())
	{
		// 디코딩 프레임 아이템 하나 획득
		DecodeFrameQueue::DecodeFrameItem* frameItem = m_decodeFrameQueue->AcquireReadFrame();
		if (!frameItem)
		{
			break;
		}

		if (frameItem->size > 0)
		{
			// 디코딩 요청
			if (m_decoder->Parse(frameItem->data, static_cast<uint32_t>(frameItem->size)))
			{
				// 디코딩 결과 프레임 동기화 후 획득
				while (D3D11NvDecoder::Frame* frame = m_decoder->GetFrame())
				{
					// 콜백 호출
					if (m_frameCallback)
					{
						m_frameCallback(*frame, m_frameCallbackUserData);
					}
				}
			}
		}

		// 디코딩 프레임 아이템 반환
		m_decodeFrameQueue->ReleaseReadFrame();
	}
}
