#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "NvEncConfig.h"
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


// 공유 입력 풀의 keyed mutex 규약.
//
// 생산자와 반드시 같은 값이어야 한다.
// (D3D11DuplicateEngine 의 FRAME_POOL_MUTEX_KEY 와 짝이다)
//
// 키를 하나만 쓰는 이유는 생산자 쪽 주석에 있다 — 요약하면 풀이
// latest-only 로 소비되어 "소비자가 손도 대지 않은 프레임" 이 정상
// 경로이고, 핑퐁 키는 그 경우에 슬롯을 영구히 죽인다.
constexpr UINT64 NVENC_SHARED_INPUT_MUTEX_KEY = 0;
constexpr DWORD  NVENC_SHARED_INPUT_MUTEX_TIMEOUT_MS = 100;

class D3D11_NVIDIA_ENCODER_API D3D11NvEncoder
{
public:
	using EncodedPacketCallback = void (*)(const NvEncPacket& packet, void* userData);
	using ErrorCallback = void (*)(NvEncErrorCode errorCode, void* userData);

	// 매 프레임 '이번 것을 키프레임으로 할까' 를 묻는다. 큐 펌프가 호출한다.
	using KeyFrameRequestCallback = bool (*)(void* userData);

	// =====================================================================
	// 생성 / 소멸
	// =====================================================================
	D3D11NvEncoder();
	~D3D11NvEncoder();

	D3D11NvEncoder(const D3D11NvEncoder&) = delete;
	D3D11NvEncoder& operator=(const D3D11NvEncoder&) = delete;

	// =====================================================================
	// 초기화 / 종료
	// =====================================================================

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
		const NvEncConfig& config,
		ID3D11ImmediateContextGate* contextGate);

	// 기본 설정으로 초기화하는 짧은 형태.
	// latencyMode = UltraLow, profile = High, intra refresh 켜짐이 적용된다.
	bool Initialize(
		ID3D11Device* device,
		uint32_t width,
		uint32_t height,
		uint32_t encodeSlotCount,
		ID3D11ImmediateContextGate* contextGate,
		bool enableAsyncPipeline = true);

	void Destroy();

	// =====================================================================
	// 재설정
	// =====================================================================

	// 인코딩 중에 설정을 바꾼다. 네트워크 상황에 맞춘 비트레이트 조정이 주 용도다.
	//
	// NvEncConfig 에서 [runtime] 으로 표시된 필드만 바꿀 수 있다.
	// [init] 필드가 하나라도 달라지면 InitOnlyFieldChanged 를 반환하고
	// 아무것도 적용하지 않는다. 그 경우 Destroy 후 재초기화해야 한다.
	//
	// forceIdr 은 변경 직후 IDR 을 강제한다. 비트레이트만 낮추는 경우라면
	// false 로 두는 편이 낫다. IDR 은 P 프레임의 몇 배 크기라 혼잡을 악화시킨다.
	//
	// 임의의 스레드에서 호출해도 되며 인코딩을 멈추지 않는다.
	NvEncReconfigureResult Reconfigure(const NvEncConfig& config, bool forceIdr = false);

	// =====================================================================
	// 공유 입력 풀 (생산자 디바이스 연결)
	// =====================================================================

	// --- 다른 디바이스가 만든 공유 텍스처 풀에서 입력받기 ---
	//
	// 왜 필요한가
	//   캡처와 인코더가 같은 D3D11 디바이스를 쓰면 immediate context 를
	//   공유하게 되고, 그 컨텍스트를 지키는 게이트 하나를 두 스레드가
	//   다툰다. 인코더가 그 게이트를 쥔 채 nvEncMapInputResource 안에서
	//   드라이버 대기에 들어가면 캡처는 영원히 멈춘다. QHD 60fps 에서
	//   실제로 그렇게 교착했다.
	//
	//   디바이스를 나누면 컨텍스트가 서로 독립이라 그 순환이 없다. 대신
	//   캡처 결과를 건네받을 길이 필요하고, 그게 이 공유 풀이다.
	//
	// 사용법
	//   Initialize 직후 한 번만 부른다. 여기서 핸들마다
	//   OpenSharedResource1 과 IDXGIKeyedMutex 조회를 끝내 두므로,
	//   매 프레임에는 슬롯 번호만 넘기면 된다. 매 프레임 여는 것은
	//   그 자체가 드라이버 왕복이라 의미가 없다.
	//
	//   handles 는 생산자(D3D11DuplicateEngine::GetFramePoolSharedHandle)가
	//   준 NT 핸들이고, 이 호출이 자기 참조를 따로 잡으므로 생산자가
	//   먼저 닫아도 된다.
	bool RegisterSharedInputPool(const HANDLE* sharedHandles, uint32_t count);

	// =====================================================================
	// 인코드 스레드 제어
	// =====================================================================

	// 큐에서 프레임을 꺼내 이 엔코더에 밀어 넣는 워커를 시작한다.
	// 인코딩 결과는 SetEncodedPacketCallback 으로 등록한 콜백에 그대로 도착한다.
	// async 파이프라인이 켜진 엔코더에서만 동작한다 — 동기 모드는 아무도 출력을
	// 회수하지 않아 정지하므로 거절한다.
	// 동기 인코딩은 StageFrame + EncodeSync 를 호출자가 직접 돌린다.
	//
	// Destroy 가 자동으로 멈추므로 종료 순서를 신경 쓸 필요가 없다.
	bool StartEncodeThread();
	void StopEncodeThread();

	// 인코드 스레드가 꺼내 갈 프레임을 넣는다.
	//
	// 유입 큐는 latest-only 다. 아직 처리되지 않은 프레임이 있으면 그것을
	// 버리고 이 프레임으로 교체한다 — 인코더가 밀렸을 때 오래된 화면을
	// 내보내는 것보다 최신 화면을 내보내는 편이 낫기 때문이다.
	// 버려진 프레임은 SetFrameReleaseCallback 콜백으로 돌아간다.
	//
	// 동기 파이프라인(enableAsyncPipeline = false)에서는 큐가 없어 항상 false 다.
	bool EnqueueFrame(const NvEncInputFrame& frame, bool forceKeyFrame);

	// =====================================================================
	// 콜백 등록
	// =====================================================================
	void SetEncodedPacketCallback(EncodedPacketCallback callback, void* userData);

	// 파이프라인이 프레임을 유실하거나 정지했을 때 통지받는다.
	// 콜백은 엔코더 완료 스레드에서 호출되므로 블로킹 작업을 하면 안 된다.
	void SetErrorCallback(ErrorCallback callback, void* userData);

	void SetKeyFrameRequestCallback(KeyFrameRequestCallback callback, void* userData);

	// EnqueueFrame 으로 넘긴 프레임을 인코더가 다 썼거나 버렸을 때 부른다.
	// 앱이 그 시점에 원본 자원(캡처 슬롯 등)을 반납한다.
	void SetFrameReleaseCallback(NvEncFrameReleaseCallback callback, void* userData);

	// =====================================================================
	// 프레임 투입
	// =====================================================================
	bool StageFrame(ID3D11Texture2D* bgraTexture);

	// 공유 풀의 slot 번째 텍스처를 인코더 입력으로 가져온다.
	// StageFrame 과 하는 일은 같고, 들어오는 텍스처가 이 디바이스 것이
	// 아니라 공유 텍스처라는 점만 다르다.
	//
	// keyed mutex 는 이 안에서 잡았다 놓는다. 복사가 끝나면 인코더는
	// 자기 BGRA 사본을 갖게 되므로 공유 텍스처를 더 붙들 이유가 없다 —
	// 뒤따르는 NV12 변환과 NVENC 제출은 전부 이 디바이스 안의 일이다.
	// 그래서 블로킹하는 구간에는 공유 자원이 걸려 있지 않다.
	bool StageFrameFromSharedSlot(uint32_t slot);

	void RequestKeyFrame();
	bool SubmitFrame(uint64_t frameId);
	bool EncodeSync(NvEncPacket& encodeResultPacket);
	bool WaitForPendingFrames(uint32_t timeoutMilliseconds = 20'000U) const;

	// =====================================================================
	// 통계 / 진단
	// =====================================================================
	void GetStats(NvEncStats& stats) const;

	// 테스트 전용 훅. 다음 count 번의 비트스트림 회수를 강제로 실패시킨다.
	// 출력 실패 복구 경로를 검증하기 위한 것으로 운영 코드에서 호출하지 않는다.
	void DebugFailNextOutputs(uint32_t count);

	// =====================================================================
	// 상태 / 설정 조회
	// =====================================================================

	// 현재 적용된 설정을 읽는다. Reconfigure 로 일부만 바꿀 때
	// 이 값을 받아 수정해서 되돌려주는 방식이 안전하다.
	void GetConfig(NvEncConfig& config) const;

	bool CanSubmitFrame() const;
	uint32_t GetPendingFrameCount() const;
	bool IsAsyncPipelineEnabled() const;

	// 파이프라인이 정지했는지 확인한다. true 면 이후 SubmitFrame 은 모두 실패하며
	// 복구하려면 Destroy 후 다시 Initialize 해야 한다.
	bool IsFaulted() const;

private:
	D3D11NvEncoder_Impl* m_impl = nullptr;
};
