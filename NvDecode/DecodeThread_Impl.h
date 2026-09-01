#pragma once

#include "../../Core/Concurrency/ThreadBase.h"
#include "D3D11NvDecoder.h"
#include "DecodeThread.h"

class DecodeFrameQueue;

class DecodeThread_Impl final : public Core::Concurrency::ThreadBase
{
public:
	using FrameCallback = DecodeThread::FrameCallback;

	DecodeThread_Impl();
	~DecodeThread_Impl();

	bool Initialize(DecodeFrameQueue* queue, D3D11NvDecoder* decoder);
	void Shutdown();
	void SetFrameCallback(FrameCallback callback, void* userData);
	void GetStats(DecodeThread::Stats& stats) const;

private:
	void Run() override;

	// 콜백을 잠금 아래에서 호출한다. 호출 중에 userData 가 교체되지 않도록.
	void DispatchFrame(const D3D11NvDecoder::Frame& frame);

private:
	DecodeFrameQueue* m_decodeFrameQueue = nullptr;
	D3D11NvDecoder* m_decoder = nullptr;

	// 콜백은 디코드 스레드에서 읽히고 임의의 스레드에서 설정될 수 있다.
	FrameCallback m_frameCallback = nullptr;
	void* m_frameCallbackUserData = nullptr;
	mutable SRWLOCK m_callbackLock = SRWLOCK_INIT;

	// 관측성: 큐에서 꺼냈지만 디코딩되지 않은 프레임을 센다.
	alignas(8) volatile LONG64 m_packetsParsedCount = 0;
	alignas(8) volatile LONG64 m_packetsFailedCount = 0;
	alignas(8) volatile LONG64 m_framesDeliveredCount = 0;
};
