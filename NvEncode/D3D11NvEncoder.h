#pragma once

#include "NvEncPacket.h"

#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define D3D11_NVIDIA_ENCODER_API __declspec(dllexport)
#else
#define D3D11_NVIDIA_ENCODER_API __declspec(dllimport)
#endif

struct ID3D11Device;
struct ID3D11Texture2D;
class ID3D11ImmediateContextGate;
class D3D11NvEncoder_Impl;

class D3D11_NVIDIA_ENCODER_API D3D11NvEncoder
{
public:
	using EncodedPacketCallback = void (*)(const NvEncPacket& packet, void* userData);
	using ErrorCallback = void (*)(NvEncErrorCode errorCode, void* userData);

	D3D11NvEncoder();
	~D3D11NvEncoder();

	D3D11NvEncoder(const D3D11NvEncoder&) = delete;
	D3D11NvEncoder& operator=(const D3D11NvEncoder&) = delete;

	// contextGate 는 이 device 의 immediate context 를 쓰는 모든 주체가
	// 공유하는 게이트여야 한다. 기본값을 두지 않는 이유는, 게이트를 빼먹으면
	// 컴파일은 되고 런타임에 조용히 깨지기 때문이다.
	// 단일 스레드에서만 컨텍스트를 쓴다면 명시적으로 nullptr 을 넘긴다.
	//
	// 주의: 게이트를 획득한 상태로 이 클래스의 어떤 함수도 호출하지 말 것.
	// 게이트는 재귀 획득이 불가능해 데드락이 발생한다.
	// Initialize / Destroy 도 내부에서 게이트를 잡는다.
	bool Initialize(
		ID3D11Device* device,
		uint32_t width,
		uint32_t height,
		uint32_t encodeBufferCount,
		ID3D11ImmediateContextGate* contextGate,
		bool enableAsyncPipeline = true);
	void Destroy();

	void SetEncodedPacketCallback(EncodedPacketCallback callback, void* userData);

	// 파이프라인이 프레임을 유실하거나 정지했을 때 통지받는다.
	// 콜백은 엔코더 완료 스레드에서 호출되므로 블로킹 작업을 하면 안 된다.
	void SetErrorCallback(ErrorCallback callback, void* userData);

	bool PrepareFrameForEncode(ID3D11Texture2D* bgraTexture);
	void RequestKeyFrame();
	bool CanSubmitFrame() const;
	bool SubmitFrame(uint64_t frameId);
	uint32_t GetPendingFrameCount() const;
	bool WaitForPendingFrames(uint32_t timeoutMilliseconds = 20'000U) const;
	bool IsAsyncPipelineEnabled() const;
	bool DoEncode(NvEncPacket& encodeResultPacket);

	// 파이프라인이 정지했는지 확인한다. true 면 이후 SubmitFrame 은 모두 실패하며
	// 복구하려면 Destroy 후 다시 Initialize 해야 한다.
	bool IsFaulted() const;
	void GetStats(NvEncStats& stats) const;

	// 테스트 전용 훅. 다음 count 번의 비트스트림 회수를 강제로 실패시킨다.
	// 출력 실패 복구 경로를 검증하기 위한 것으로 운영 코드에서 호출하지 않는다.
	void DebugFailNextOutputs(uint32_t count);

private:
	D3D11NvEncoder_Impl* m_impl = nullptr;
};
