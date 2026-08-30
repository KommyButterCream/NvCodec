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
		m_encoder->WaitForPendingFrames();
		m_encoder->SetEncodedPacketCallback(nullptr, nullptr);
	}

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
	while (!IsStopRequested())
	{
		EncodeFrameQueue::EncodeFrameItem* frameItem = m_encodeFrameQueue->AcquireReadFrame();
		if (!frameItem)
			break;

		const uint64_t frameId = frameItem->frameHandle.frameId;
		bool forceKeyFrame = frameItem->forceKeyFrame;
		if (m_keyFrameRequestCallback && m_keyFrameRequestCallback(m_keyFrameRequestCallbackUserData))
		{
			forceKeyFrame = true;
		}

		if (forceKeyFrame)
		{
			// Keep the request pending even when this frame is dropped because all
			// encoder slots are busy. EncodeFrame consumes it on actual submission.
			m_encoder->RequestKeyFrame();
		}

		if (!m_encoder->CanSubmitFrame())
		{
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
			continue;

		if (!m_encoder->SubmitFrame(frameId))
			continue;
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
	if (!m_encodedFrameCallback || !packet.data || packet.size == 0)
		return;

	EncodeThread::EncodedFrame encodedFrame = {};
	encodedFrame.data = packet.data;
	encodedFrame.size = packet.size;
	encodedFrame.frameId = packet.frameId;
	encodedFrame.timestamp = packet.timestamp;
	encodedFrame.frameType = packet.frameType;
	encodedFrame.isKeyFrame = packet.isKeyFrame;

	m_encodedFrameCallback(encodedFrame, m_encodedFrameCallbackUserData);
}
