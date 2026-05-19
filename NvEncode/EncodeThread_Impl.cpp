#include "pch.h"
#include "EncodeThread_Impl.h"

#include "D3D11NvEncoder.h"
#include "EncodeFrameQueue.h"

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

	Shutdown();

	// 외부로부터 EncodeFrameQueue 와 NvEncoder 입력 및 설정
	m_encodeFrameQueue = queue;
	m_encoder = encoder;

	// 엔코드 스레드 시작
	if (!Start())
	{
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

	m_encodeFrameQueue = nullptr;
	m_encoder = nullptr;
}

void EncodeThread_Impl::SetEncodedFrameCallback(EncodeThread::EncodedFrameCallback callback, void* userData)
{
	// 엔코딩 끝난 후 호출될 콜백 함수 설정
	m_encodedFrameCallback = callback;
	m_encodedFrameCallbackUserData = userData;
}

void EncodeThread_Impl::SetKeyFrameRequestCallback(EncodeThread::KeyFrameRequestCallback callback, void* userData)
{
	// 키프레임을 포함하여 엔코딩을 할 것인지 확인하는 콜백 함수 설정
	m_keyFrameRequestCallback = callback;
	m_keyFrameRequestCallbackUserData = userData;
}

void EncodeThread_Impl::Run()
{
	// 스레드
	while (!IsStopRequested())
	{
		// 엔코딩 프레임 아이템 하나 획득
		EncodeFrameQueue::EncodeFrameItem* frameItem = m_encodeFrameQueue->AcquireReadFrame();
		if (!frameItem)
			break;

		// Capture Engine 에서 설정된 FrameID 값 읽어오기
		const uint64_t frameId = frameItem->frameHandle.frameId;

		// KeyFrame 이 필요한지 확인하고 필요한 경우 엔코더에 키 프레임 Request 설정
		// 이후 PrepareFrameForEncode 에서 엔코더 버퍼에 텍스쳐 복사하여 엔코딩 준비
		bool prepareSucceeded = false;
		if (frameItem->frameHandle.texture)
		{
			// 프레임 핸들에 텍스쳐가 있는 경우에만 수행
			bool forceKeyFrame = frameItem->forceKeyFrame;
			if (m_keyFrameRequestCallback && m_keyFrameRequestCallback(m_keyFrameRequestCallbackUserData))
			{
				forceKeyFrame = true;
			}

			if (forceKeyFrame)
			{
				m_encoder->RequestKeyFrame();
			}

			// 엔코더 내부 버퍼에 텍스쳐를 복사하여 준비
			prepareSucceeded = m_encoder->PrepareFrameForEncode(frameItem->frameHandle.texture);
		}

		// 디코딩 프레임 아이템 반환
		// PrepareFrameForEncode 에서 내부 버퍼로 복사를 수행했기 때문에 더이상 필요가 없다.
		// 다음 프레임 캡쳐에 필요한 버퍼를 준비하기 위해 미리 Release 해준다.
		m_encodeFrameQueue->ReleaseReadFrame();

		if (!prepareSucceeded)
			continue;

		// 엔코딩 요청
		// 동기식으로 작동하므로 엔코딩 결과를 가져온다.
		NvEncPacket encodeResultPacket = {};
		if (!m_encoder->DoEncode(encodeResultPacket))
			continue;

		if (!encodeResultPacket.data || encodeResultPacket.size == 0)
			continue;

		// 앤코딩 완료 이후에 수행 되어야 할 콜백 호출
		if (m_encodedFrameCallback)
		{
			EncodeThread::EncodedFrame encodedFrame = {};
			encodedFrame.data = encodeResultPacket.data;
			encodedFrame.size = encodeResultPacket.size;
			encodedFrame.frameId = frameId;
			encodedFrame.timestamp = encodeResultPacket.timestamp;
			encodedFrame.frameType = encodeResultPacket.frameType;
			encodedFrame.isKeyFrame = encodeResultPacket.isKeyFrame;

			m_encodedFrameCallback(encodedFrame, m_encodedFrameCallbackUserData);
		}
	}
}
