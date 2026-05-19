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

	using EncodedFrameCallback = void (*)(const EncodedFrame& frame, void* userData);
	using KeyFrameRequestCallback = bool (*)(void* userData);

public:
	EncodeThread();
	~EncodeThread();

	bool Initialize(EncodeFrameQueue* queue, D3D11NvEncoder* encoder);
	void Shutdown();

	void SetEncodedFrameCallback(EncodedFrameCallback callback, void* userData);
	void SetKeyFrameRequestCallback(KeyFrameRequestCallback callback, void* userData);

private:
	EncodeThread_Impl* m_impl = nullptr;
};
