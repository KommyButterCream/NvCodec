#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11_1.h>

#include <cstdint>

#include "NvDecConfig.h"

#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define D3D11_NVIDIA_DECODER_API __declspec(dllexport)
#else
#define D3D11_NVIDIA_DECODER_API __declspec(dllimport)
#endif

class ID3D11ImmediateContextGate;
class D3D11NvDecoder_Impl;

class D3D11_NVIDIA_DECODER_API D3D11NvDecoder
{
public:
	// 디코딩이 끝난 프레임 한 장.
	//
	// AcquireFrame 이 돌려준 뒤부터 ReleaseFrame 을 부를 때까지 texture 는
	// 디코더가 덮어쓰지 않는다. 반납하기 전까지 렌더링에 그대로 써도 된다.
	// 반납을 빼먹으면 그 슬롯이 영구히 묶여 이후 프레임이 버려진다.
	struct Frame
	{
		ID3D11Texture2D* texture = nullptr;
		HANDLE sharedHandle = nullptr;
		uint64_t timestamp = 0;

		// 내부 슬롯 식별자. 해석하지 말고 ReleaseFrame 에 그대로 넘긴다.
		uint32_t slot = 0;
	};

	using ErrorCallback = void (*)(NvDecErrorCode errorCode, void* userData);

	// 디코드 스레드가 프레임 하나를 완성할 때마다 부른다.
	// frame 은 콜백이 반환할 때까지만 유효하다 — 스레드가 곧바로 반납한다.
	// 텍스처를 콜백 밖으로 들고 나가려면 이 스레드를 쓰지 말고
	// AcquireFrame / ReleaseFrame 을 직접 호출해야 한다.
	using FrameCallback = void (*)(const Frame& frame, void* userData);

	// =====================================================================
	// 생성 / 소멸
	// =====================================================================
	D3D11NvDecoder();
	~D3D11NvDecoder();

	D3D11NvDecoder(const D3D11NvDecoder&) = delete;
	D3D11NvDecoder& operator=(const D3D11NvDecoder&) = delete;

	// =====================================================================
	// 초기화 / 종료
	// =====================================================================

	// contextGate 는 이 device 의 immediate context 를 쓰는 모든 주체가
	// 공유하는 게이트여야 한다. 디코더는 CUDA-D3D11 interop 으로
	// 매 프레임 D3D11 리소스를 매핑하므로, 렌더 스레드와 같은 컨텍스트를
	// 쓴다면 게이트 없이는 보호되지 않는다.
	// 단일 스레드에서만 컨텍스트를 쓴다면 명시적으로 nullptr 을 넘긴다.
	//
	// 주의: 게이트를 획득한 상태로 이 클래스의 어떤 함수도 호출하지 말 것.
	// 게이트는 재귀 획득이 불가능해 데드락이 발생한다.
	bool Initialize(
		ID3D11Device* device,
		const NvDecConfig& config,
		ID3D11ImmediateContextGate* contextGate);

	// 기본 설정으로 초기화하는 짧은 형태.
	bool Initialize(
		ID3D11Device* device,
		ID3D11ImmediateContextGate* contextGate,
		bool sharedOutputTextureMode = false);

	void Destroy();

	// =====================================================================
	// 디코드 스레드 제어
	// =====================================================================

	// 큐에서 패킷을 꺼내 이 디코더에 먹이는 워커를 시작한다.
	// 결과는 SetFrameCallback 으로 등록한 콜백에 도착한다.
	// Destroy 가 자동으로 멈추므로 종료 순서를 신경 쓸 필요가 없다.
	bool StartDecodeThread();
	void StopDecodeThread();

	// 디코드 스레드가 꺼내 갈 비트스트림을 넣는다.
	//
	// 큐가 자기 버퍼로 복사하므로 frame.data 는 이 호출이 반환하면
	// 재사용해도 된다. 큐가 가득 차면 가장 오래된 것이 버려지고,
	// 그 수는 GetStats 의 droppedInputQueue 로 나온다.
	bool EnqueueFrame(const NvDecInputFrame& frame);

	// =====================================================================
	// 콜백 등록
	// =====================================================================
	void SetFrameCallback(FrameCallback callback, void* userData);

	// 프레임을 유실하거나 파이프라인이 정지했을 때 통지받는다.
	// 콜백은 디코드 스레드에서 호출되므로 블로킹 작업을 하면 안 된다.
	void SetErrorCallback(ErrorCallback callback, void* userData);

	// =====================================================================
	// 비트스트림 투입
	// =====================================================================

	// timestamp 는 NVDEC 를 그대로 통과해 Frame::timestamp 로 돌아온다.
	// 앱이 디코딩 결과를 원본 프레임과 짝짓는 유일한 수단이다.
	// (예전에는 CUVID_PKT_TIMESTAMP 플래그만 세우고 값을 넣지 않아 항상 0 이었다.)
	bool Parse(const uint8_t* data, uint32_t size, uint64_t timestamp = 0,
		bool endOfPicture = true, bool endOfStream = false, bool discontinuity = false);

	// =====================================================================
	// 프레임 수신
	// =====================================================================

	// 준비된 프레임을 하나 꺼낸다. 없으면 nullptr.
	// 반환된 프레임은 반드시 ReleaseFrame 으로 돌려줘야 한다.
	Frame* AcquireFrame();

	// AcquireFrame 으로 받은 프레임을 반납한다. 슬롯이 다시 쓰인다.
	void ReleaseFrame(Frame* frame);

	// =====================================================================
	// 통계 / 상태 조회
	// =====================================================================
	void GetStats(NvDecStats& stats) const;

	// true 면 이후 디코딩이 진행되지 않는다. 복구하려면 Destroy 후 재초기화.
	bool IsFaulted() const;

private:
	D3D11NvDecoder_Impl* m_impl = nullptr;
};
