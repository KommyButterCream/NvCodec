#pragma once

#include "D3D11NvDecoder.h"

class DecodeFrameQueue;
class DecodeThread_Impl;

class D3D11_NVIDIA_DECODER_API DecodeThread
{
public:
	// 큐에서 꺼낸 패킷이 어디서 사라졌는지 구분하기 위한 통계.
	struct Stats
	{
		uint64_t packetsParsed = 0;
		uint64_t packetsFailed = 0;
		uint64_t framesDelivered = 0;
	};

	// frame 은 콜백이 반환할 때까지만 유효하다.
	// 콜백 밖으로 텍스처를 들고 나가려면 이 스레드 대신
	// D3D11NvDecoder::AcquireFrame / ReleaseFrame 을 직접 써야 한다.
	using FrameCallback = void (*)(const D3D11NvDecoder::Frame& frame, void* userData);

	DecodeThread();
	~DecodeThread();

	bool Initialize(DecodeFrameQueue* queue, D3D11NvDecoder* decoder);
	void Shutdown();
	void SetFrameCallback(FrameCallback callback, void* userData);
	void GetStats(Stats& stats) const;

private:
	DecodeThread_Impl* m_impl = nullptr;
};
