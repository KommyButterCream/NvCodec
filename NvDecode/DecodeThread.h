#pragma once

#include "../../Core/Concurrency/ThreadBase.h"

#include "D3D11NvDecoder.h"

class D3D11NvDecoder_Impl;
class DecodeFrameQueue;

// 큐에서 패킷을 꺼내 디코더에 먹이고, 나온 프레임을 콜백으로 넘기는 워커.
//
// DLL 내부 전용이다. 예전에는 공개 클래스라 pimpl 로 나뉘어 있었지만,
// 지금은 D3D11NvDecoder 가 소유하고 StartDecodeThread 로 시작한다.
//
// AcquireFrame 으로 받은 프레임은 콜백이 반환하는 즉시 ReleaseFrame 으로
// 반납한다. 콜백 밖으로 텍스처를 들고 나가려면 이 스레드를 쓰지 말고
// 앱이 직접 AcquireFrame / ReleaseFrame 을 호출해야 한다.
class DecodeThread final : public Core::Concurrency::ThreadBase
{
public:
	using FrameCallback = D3D11NvDecoder::FrameCallback;

	DecodeThread();
	~DecodeThread();

	DecodeThread(const DecodeThread&) = delete;
	DecodeThread& operator=(const DecodeThread&) = delete;

	bool Initialize(DecodeFrameQueue* queue, D3D11NvDecoder_Impl* decoder);
	void Shutdown();

	void SetFrameCallback(FrameCallback callback, void* userData);

	// 큐에서 꺼냈지만 디코딩되지 않은 패킷 수를 채운다.
	// 나머지 필드는 건드리지 않는다 — 디코더가 자기 값을 이미 채웠다.
	void FillStats(NvDecStats& stats) const;

private:
	void Run() override;

	// 콜백을 잠금 아래에서 호출한다. 호출 중에 userData 가 교체되지 않도록.
	void DispatchFrame(const D3D11NvDecoder::Frame& frame);

private:
	DecodeFrameQueue* m_decodeFrameQueue = nullptr;
	D3D11NvDecoder_Impl* m_decoder = nullptr;

	FrameCallback m_frameCallback = nullptr;
	void* m_frameCallbackUserData = nullptr;
	mutable SRWLOCK m_callbackLock = SRWLOCK_INIT;

	alignas(8) volatile LONG64 m_packetsFailedCount = 0;
};
