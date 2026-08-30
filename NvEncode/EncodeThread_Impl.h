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

private:
	void Run() override;

private:
	EncodeFrameQueue* m_encodeFrameQueue = nullptr;
	D3D11NvEncoder* m_encoder = nullptr;
	EncodeThread::EncodedFrameCallback m_encodedFrameCallback = nullptr;
	void* m_encodedFrameCallbackUserData = nullptr;
	EncodeThread::KeyFrameRequestCallback m_keyFrameRequestCallback = nullptr;
	void* m_keyFrameRequestCallbackUserData = nullptr;
};
