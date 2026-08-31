#pragma once

#include "../../Core/Concurrency/ThreadBase.h"
#include "D3D11NvDecoder.h"

class DecodeFrameQueue;

class DecodeThread_Impl final : public Core::Concurrency::ThreadBase
{
public:
	using FrameCallback = void (*)(const D3D11NvDecoder::Frame& frame, void* userData);

	DecodeThread_Impl();
	~DecodeThread_Impl();

	bool Initialize(DecodeFrameQueue* queue, D3D11NvDecoder* decoder);
	void Shutdown();
	void SetFrameCallback(FrameCallback callback, void* userData);

private:
	void Run() override;

private:
	DecodeFrameQueue* m_decodeFrameQueue = nullptr;
	D3D11NvDecoder* m_decoder = nullptr;
	FrameCallback m_frameCallback = nullptr;
	void* m_frameCallbackUserData = nullptr;
};
