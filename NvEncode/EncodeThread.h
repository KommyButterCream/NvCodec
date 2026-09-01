#pragma once

#include "../../Core/Concurrency/ThreadBase.h"

#include "NvEncPacket.h"

class D3D11NvEncoder_Impl;
class EncodeFrameQueue;

// 큐에서 프레임을 꺼내 인코더에 밀어 넣는 워커.
//
// DLL 내부 전용이다. 예전에는 공개 클래스라 pimpl 로 나뉘어 있었고, 그 껍데기가
// 엔코더의 패킷 콜백을 가로채 자기 콜백으로 다시 뿌렸다. 그 중간 구조체는
// NvEncPacket 과 필드가 한 글자도 다르지 않았다.
// 지금은 D3D11NvEncoder 가 이 스레드를 소유하고, 결과는 앱이 엔코더에 직접
// 등록한 SetEncodedPacketCallback 으로 바로 간다.
//
// 출력 회수는 이 스레드가 하지 않는다. 그건 EncodeCompletionThread 담당이고,
// 그래서 async 파이프라인이 켜진 엔코더에서만 동작한다.
class EncodeThread final : public Core::Concurrency::ThreadBase
{
public:
	using KeyFrameRequestCallback = bool (*)(void* userData);

	EncodeThread();
	~EncodeThread();

	EncodeThread(const EncodeThread&) = delete;
	EncodeThread& operator=(const EncodeThread&) = delete;

	bool Initialize(EncodeFrameQueue* queue, D3D11NvEncoder_Impl* encoder);
	void Shutdown();

	void SetKeyFrameRequestCallback(KeyFrameRequestCallback callback, void* userData);

	// 큐에서 꺼냈지만 인코더에 넣지 못한 프레임 수를 채운다.
	// 나머지 필드는 건드리지 않는다 — 엔코더가 자기 값을 이미 채웠다.
	void FillStats(NvEncStats& stats) const;

private:
	void Run() override;
	bool QueryKeyFrameRequest();

private:
	EncodeFrameQueue* m_encodeFrameQueue = nullptr;
	D3D11NvEncoder_Impl* m_encoder = nullptr;

	// 엔코드 스레드가 읽고 임의의 스레드가 설정할 수 있으므로 잠금이 필요하다.
	KeyFrameRequestCallback m_keyFrameRequestCallback = nullptr;
	void* m_keyFrameRequestCallbackUserData = nullptr;
	mutable SRWLOCK m_callbackLock = SRWLOCK_INIT;

	alignas(8) volatile LONG64 m_droppedNoEncoderSlotCount = 0;
	alignas(8) volatile LONG64 m_droppedPrepareFailedCount = 0;
	alignas(8) volatile LONG64 m_droppedSubmitFailedCount = 0;
};
