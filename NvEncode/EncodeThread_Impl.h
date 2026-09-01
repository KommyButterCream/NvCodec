#pragma once

#include "../../Core/Concurrency/ThreadBase.h"
#include "EncodeThread.h"

class D3D11NvEncoder;
class EncodeFrameQueue;

class EncodeThread_Impl final : public Core::Concurrency::ThreadBase
{
public:
	EncodeThread_Impl();
	~EncodeThread_Impl();

	bool Initialize(EncodeFrameQueue* queue, D3D11NvEncoder* encoder);
	void Shutdown();

	void SetEncodedFrameCallback(EncodeThread::EncodedFrameCallback callback, void* userData);
	void SetKeyFrameRequestCallback(EncodeThread::KeyFrameRequestCallback callback, void* userData);
	void GetStats(EncodeThread::Stats& stats) const;
	bool QueryKeyFrameRequest();

private:
	void Run() override;
	static void EncodedPacketCallback(const NvEncPacket& packet, void* userData);
	void DispatchEncodedFrame(const NvEncPacket& packet);

private:
	EncodeFrameQueue* m_encodeFrameQueue = nullptr;
	D3D11NvEncoder* m_encoder = nullptr;

	// 콜백은 엔코더 완료 스레드(EncodedFrame)와 엔코드 스레드(KeyFrameRequest)에서
	// 읽히고 임의의 스레드에서 설정될 수 있으므로 잠금이 필요하다.
	EncodeThread::EncodedFrameCallback m_encodedFrameCallback = nullptr;
	void* m_encodedFrameCallbackUserData = nullptr;
	EncodeThread::KeyFrameRequestCallback m_keyFrameRequestCallback = nullptr;
	void* m_keyFrameRequestCallbackUserData = nullptr;
	SRWLOCK m_callbackLock = SRWLOCK_INIT;

	// 관측성: 큐에서 꺼냈지만 인코더에 넣지 못한 프레임을 센다.
	// EncodeFrameQueue::GetDropCount 는 enqueue 측 드롭만 세므로 여기서 따로 센다.
	alignas(8) volatile LONG64 m_submittedFrameCount = 0;
	alignas(8) volatile LONG64 m_droppedNoEncoderSlotCount = 0;
	alignas(8) volatile LONG64 m_droppedPrepareFailedCount = 0;
	alignas(8) volatile LONG64 m_droppedSubmitFailedCount = 0;
};
