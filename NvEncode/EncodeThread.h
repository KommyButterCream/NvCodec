#pragma once

#include <stdint.h>

#include "NvEncPacket.h"

#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define D3D11_NVIDIA_ENCODER_API __declspec(dllexport)
#else
#define D3D11_NVIDIA_ENCODER_API __declspec(dllimport)
#endif

class D3D11NvEncoder;
class EncodeFrameQueue;
class EncodeThread_Impl;

class D3D11_NVIDIA_ENCODER_API EncodeThread
{
public:
	struct EncodedFrame
	{
		const uint8_t* data = nullptr;
		uint32_t size = 0;
		uint64_t frameId = 0;
		uint64_t timestamp = 0;
		uint16_t frameType = 0;
		bool isKeyFrame = false;
	};

	// 큐에서 꺼낸 프레임이 어디서 사라졌는지 구분하기 위한 통계.
	// EncodeFrameQueue::GetDropCount 는 enqueue 측 드롭만 세기 때문에
	// 인코더 슬롯 부족으로 버려진 프레임은 여기에만 잡힌다.
	struct Stats
	{
		uint64_t framesSubmitted = 0;
		uint64_t framesDroppedNoEncoderSlot = 0;
		uint64_t framesDroppedPrepareFailed = 0;
		uint64_t framesDroppedSubmitFailed = 0;
	};

	using EncodedFrameCallback = void (*)(const EncodedFrame& frame, void* userData);
	using KeyFrameRequestCallback = bool (*)(void* userData);

public:
	EncodeThread();
	~EncodeThread();

	// encoder 는 반드시 Initialize 가 끝난, async 파이프라인이 켜진 엔코더여야 한다.
	// 이 스레드는 출력을 회수하지 않으므로 동기 모드 엔코더를 넘기면 거절된다.
	// 동기 인코딩은 호출자가 PrepareFrameForEncode + DoEncode 를 직접 돌린다.
	bool Initialize(EncodeFrameQueue* queue, D3D11NvEncoder* encoder);
	void Shutdown();

	void SetEncodedFrameCallback(EncodedFrameCallback callback, void* userData);
	void SetKeyFrameRequestCallback(KeyFrameRequestCallback callback, void* userData);
	void GetStats(Stats& stats) const;

private:
	EncodeThread_Impl* m_impl = nullptr;
};
