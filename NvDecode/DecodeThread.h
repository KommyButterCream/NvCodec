#pragma once

#include "D3D11NvDecoder.h"

class BitstreamRingBuffer;
class DecodeThread_Impl;

class D3D11_NVIDIA_DECODER_API DecodeThread
{
public:
	using FrameCallback = void (*)(const D3D11NvDecoder::Frame& frame, void* userData);

	DecodeThread();
	~DecodeThread();

	bool Initialize(BitstreamRingBuffer* buffer, D3D11NvDecoder* decoder);
	void Shutdown();
	void SetFrameCallback(FrameCallback callback, void* userData);

private:
	DecodeThread_Impl* m_impl = nullptr;
};
