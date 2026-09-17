#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <dxgiformat.h>

#include "../Nvidia Video Codec SDK/Interface/nvEncodeAPI.h"
#include "NvEncConfig.h"
#include "NvEncPacket.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIKeyedMutex;
class ID3D11ImmediateContextGate;
class D3D11VideoProcessorNV12;
class EncodeCompletionThread;
class EncodeFrameQueue;
class EncodeThread;

enum class NvEncPacketStatus : uint8_t
{
	Error = 0,
	NotReady,
	Ready,
};

// CompleteOldestFrame 의 결과.
// Completed : 비트스트림까지 회수하고 슬롯을 반납했다.
// NotReady  : 아직 완료되지 않음. 슬롯을 그대로 유지한다.
// FrameLost : 프레임 1장을 버렸지만 슬롯은 회수했다. 파이프라인은 계속 돈다.
// Fatal     : 슬롯을 안전하게 회수할 수 없다. 이미 faulted 상태로 진입해 있다.
enum class NvEncCompletionResult : uint8_t
{
	Completed = 0,
	NotReady,
	FrameLost,
	Fatal,
};

struct NvEncPacketBuffer
{
	uint8_t* streamData = nullptr;
	uint32_t streamDataSize = 0;
	uint32_t streamDataCapacity = 0;
	NV_ENC_PIC_TYPE pictureType = NV_ENC_PIC_TYPE::NV_ENC_PIC_TYPE_UNKNOWN;
	uint64_t timestamp = 0;
	bool isKeyFrame = false;
};

struct NvEncPendingFrame
{
	uint64_t frameId = 0;
	alignas(4) volatile LONG submitted = FALSE;
};

class D3D11NvEncoder_Impl
{
friend class EncodeCompletionThread;

public:
	using EncodedPacketCallback = void (*)(const NvEncPacket& packet, void* userData);
	using ErrorCallback = void (*)(NvEncErrorCode errorCode, void* userData);

	// =====================================================================
	// 생성 / 소멸
	// =====================================================================
	D3D11NvEncoder_Impl() = default;
	~D3D11NvEncoder_Impl();

	D3D11NvEncoder_Impl(const D3D11NvEncoder_Impl&) = delete;
	D3D11NvEncoder_Impl& operator=(const D3D11NvEncoder_Impl&) = delete;

	// =====================================================================
	// 초기화 / 종료
	// =====================================================================
	bool Initialize(
		ID3D11Device* device,
		const NvEncConfig& config,
		ID3D11ImmediateContextGate* contextGate);
	void Destroy();

	// =====================================================================
	// 재설정
	// =====================================================================
	NvEncReconfigureResult Reconfigure(const NvEncConfig& config, bool forceIdr);

	// =====================================================================
	// 공유 입력 풀 (생산자 디바이스 연결)
	// =====================================================================
	bool RegisterSharedInputPool(const HANDLE* sharedHandles, uint32_t count);
	void UnregisterSharedInputPool();

	// =====================================================================
	// 인코드 스레드 제어
	// =====================================================================

	// 큐에서 프레임을 꺼내 이 엔코더에 밀어 넣는 워커를 시작한다.
	// 결과는 SetEncodedPacketCallback 으로 이미 등록된 콜백으로 간다.
	// Destroy 가 자동으로 멈추므로 호출자가 순서를 지킬 필요가 없다.
	bool StartEncodeThread();
	void StopEncodeThread();

	// 유입. 인코드 스레드가 꺼내 간다.
	bool EnqueueFrame(const NvEncInputFrame& frame, bool forceKeyFrame);

	// =====================================================================
	// 콜백 등록
	// =====================================================================
	void SetEncodedPacketCallback(EncodedPacketCallback callback, void* userData);
	void SetErrorCallback(ErrorCallback callback, void* userData);
	void SetKeyFrameRequestCallback(bool (*callback)(void*), void* userData);
	void SetFrameReleaseCallback(NvEncFrameReleaseCallback callback, void* userData);

	// =====================================================================
	// 프레임 투입
	// =====================================================================
	bool StageFrame(ID3D11Texture2D* bgraTexture);
	bool StageFrameFromSharedSlot(uint32_t slot);
	void RequestKeyFrame();
	bool SubmitFrame(uint64_t frameId);
	bool EncodeSync(NvEncPacket& encodeResultPacket);
	bool WaitForPendingFrames(uint32_t timeoutMilliseconds) const;

	// =====================================================================
	// 통계 / 진단
	// =====================================================================
	void GetStats(NvEncStats& stats) const;
	void DebugFailNextOutputs(uint32_t count);

	// =====================================================================
	// 상태 / 설정 조회
	// =====================================================================
	void GetConfig(NvEncConfig& config) const;
	bool CanSubmitFrame() const;
	uint32_t GetPendingFrameCount() const;
	bool IsAsyncPipelineEnabled() const;
	bool IsFaulted() const;

private:
	// --- 초기화 / 리소스 생성 ---
	bool LoadNvEncApi();
	bool OpenEncodeSession();

	// 게이트를 획득한 상태에서 호출된다. 내부에서 게이트를 다시 잡아서는 안 된다.
	bool CreateEncoderResources();

	// NvEncConfig 를 NVENC 구조체로 옮긴다. Initialize 와 Reconfigure 가 공유한다.
	// [init] 필드까지 채우는 것은 Initialize 뿐이고, Reconfigure 는 rate control 만 갱신한다.
	void ApplyStaticConfig(const NvEncConfig& config);
	void ApplyRateControlConfig(const NvEncConfig& config);
	static bool StaticFieldsDiffer(const NvEncConfig& a, const NvEncConfig& b);

	bool InitializeEncoder();
	void DestroyEncoder();

	// pending / all-slots-free 동기 이벤트.
	// 완료 스레드와 수명을 분리해야 한다. 완료 스레드와 함께 만들고 지우면
	// SubmitFrame(엔코드 스레드)과 WaitForPendingFrames(임의 스레드)가
	// 이미 닫힌 핸들을 읽는 창이 생긴다.
	bool CreateSyncEvents();
	void DestroySyncEvents();

	bool CreateAsyncEvent();
	void DestroyAsyncEvent();

	bool CreateMappedInputBuffers();
	void DestroyMappedInputBuffers();

	bool CreateBitstreamBuffers();
	void DestroyBitstreamBuffers();

	bool CreateRegisteredResources();
	void DestroyRegisteredResources();

	bool CreateD3D11InputBuffers();
	void DestroyD3D11InputBuffers();

	bool CreateBGRAToNV12Converter();
	void DestroyBGRAToNV12Converter();

	bool CreatePacketBuffers();
	void DestroyPacketBuffers();
	void ReleasePacketBuffer(NvEncPacketBuffer& frame);

	bool CreatePendingFrames();
	void DestroyPendingFrames();

	bool CreateEncodeCompletionThread();
	void DestroyEncodeCompletionThread();

	bool CreateInputQueue(uint32_t depth);
	void DestroyInputQueue();
	static void QueueFrameReleaseCallback(NvEncInputFrame& frame, void* userData);

	// --- 인코드 파이프라인 ---
	bool MapInputResource(uint32_t slot);
	bool UnmapInputResource(uint32_t slot);

	bool EncodePicture(uint32_t slot);
	NvEncPacketStatus WaitForEncodeCompletion(uint32_t slot, bool block);
	bool ReadEncodedBitstream(uint32_t slot, NvEncPacket& packet);
	bool Flush();

	// 가장 오래된 제출 프레임 하나를 끝낸다 — 완료 대기, 비트스트림 회수,
	// 입력 리소스 unmap, 슬롯 반납까지. 완료 스레드와 동기 경로가 함께 쓴다.
	NvEncCompletionResult CompleteOldestFrame(bool block, bool invokeCallback, NvEncPacket* outPacket = nullptr);
	void ClearPendingFrame(uint32_t slot);
	void AbortPendingFrames();
	void SignalAllSlotsFree();

	// --- 입력 리소스 등록 ---
	bool RegisterResource(void* buffer, NV_ENC_INPUT_RESOURCE_TYPE eResourceType,
		uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT eBufferFormat, NV_ENC_BUFFER_USAGE eBufferUsage,
		NV_ENC_REGISTERED_PTR& registeredResource);

	bool RegisterInputResources(void** inputFrames, uint32_t inputFrameCount, NV_ENC_INPUT_RESOURCE_TYPE eResourceType,
		uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT eBufferFormat);

	bool SetNV12OutputTexture(ID3D11Texture2D** textures, uint32_t bufferCount);
	bool SetBGRAInputTexture(ID3D11Texture2D** textures, uint32_t bufferCount);

	// --- 오류 / 콜백 통지 ---
	void EnterFaultedState(NvEncErrorCode errorCode);
	void InvokeEncodedPacketCallback(const NvEncPacket& packet);
	void InvokeErrorCallback(NvEncErrorCode errorCode);

	// --- 진단 ---
	bool ConsumeDebugOutputFailure();

	// --- 조회 ---
	uint32_t GetInputSlotIndex() const;
	uint32_t GetOutputSlotIndex() const;
	HANDLE GetCompletionEvent(uint32_t slot);

	int32_t GetCapabilityValue(GUID guidCodec, NV_ENC_CAPS capsToQuery);
	uint32_t GetEncodeWidth() const;
	uint32_t GetEncodeHeight() const;
	uint32_t GetMaxEncodeWidth() const;
	uint32_t GetMaxEncodeHeight() const;
	NV_ENC_BUFFER_FORMAT GetPixelFormat() const;
	DXGI_FORMAT GetD3D11Format(NV_ENC_BUFFER_FORMAT eBufferFormat) const;

private:
	// =====================================================================
	// 초기화 시점에 정해지고 이후 바뀌지 않는 것들
	// =====================================================================
	ID3D11Device* m_D3D11Device = nullptr;
	ID3D11DeviceContext* m_D3D11Context = nullptr;
	ID3D11ImmediateContextGate* m_contextGate = nullptr;

	void* m_encoderHandle = nullptr;
	NV_ENCODE_API_FUNCTION_LIST m_nvenc = {};

	NV_ENC_INITIALIZE_PARAMS m_initParameters = {};
	NV_ENC_CONFIG m_config = {};

	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_encodeSlotCount = 1;
	bool m_asyncPipelineEnabled = true;

	// =====================================================================
	// 슬롯 리소스
	// =====================================================================
	D3D11VideoProcessorNV12* m_converter = nullptr;

	NvEncPacketBuffer* m_packetBuffers = nullptr;
	NvEncPendingFrame* m_pendingFrames = nullptr;
	NV_ENC_REGISTERED_PTR* m_registeredResources = nullptr;
	NV_ENC_INPUT_PTR* m_mappedInputBuffers = nullptr;
	NV_ENC_OUTPUT_PTR* m_bitstreamBuffers = nullptr;

	ID3D11Texture2D** m_bgraTextures = nullptr;
	ID3D11Texture2D** m_nv12Textures = nullptr;

	// 생산자 디바이스가 만든 공유 텍스처를 이 디바이스에서 연 결과.
	// RegisterSharedInputPool 에서 한 번 채우고 Destroy 까지 그대로 둔다.
	ID3D11Texture2D** m_sharedInputTextures = nullptr;
	IDXGIKeyedMutex** m_sharedInputMutexes = nullptr;
	uint32_t m_sharedInputCount = 0;

	// =====================================================================
	// 이벤트 / 스레드
	// =====================================================================
	HANDLE* m_slotCompletionEvents = nullptr;
	HANDLE m_eosCompletionEvent = nullptr;

	// Initialize 에서 만들고 Destroy 끝에서 닫는다. 엔코더 수명 전체에 걸쳐 유효.
	HANDLE m_allSlotsFreeEvent = nullptr;
	HANDLE m_frameSubmittedEvent = nullptr;

	EncodeCompletionThread* m_encodeCompletionThread = nullptr;

	// 큐 펌프. 앱이 StartEncodeThread 를 부를 때만 생성된다.
	// 동기 인코딩(StageFrame + EncodeSync)에서는 nullptr 로 남는다.
	EncodeThread* m_encodeThread = nullptr;

	// 유입 큐. 소비자가 이 인코더 하나뿐이라 인코더가 소유한다.
	// async 파이프라인일 때만 만들어진다.
	EncodeFrameQueue* m_inputQueue = nullptr;

	// =====================================================================
	// 투입 측 — 엔코드 스레드가 쓴다
	// =====================================================================
	alignas(64) uint64_t m_timestamp = 0;
	uint32_t m_inputSequence = 0;
	volatile LONG64 m_submittedFrameCount = 0;

	// =====================================================================
	// 완료 측 — 완료 스레드가 쓴다
	// =====================================================================
	alignas(64) uint32_t m_outputSequence = 0;
	volatile LONG64 m_completedFrameCount = 0;
	volatile LONG64 m_lostFrameCount = 0;

	// =====================================================================
	// 양쪽이 함께 갱신하는 동기 카운터
	// =====================================================================
	alignas(64) volatile LONG m_pendingFrameCount = 0;

	// =====================================================================
	// 읽기 위주 플래그
	// =====================================================================
	alignas(64) volatile LONG m_forceKeyFrame = FALSE;
	volatile LONG m_acceptFrames = FALSE;
	volatile LONG m_faulted = FALSE;
	volatile LONG m_debugFailOutputCount = 0;

	NvEncFrameReleaseCallback m_frameReleaseCallback = nullptr;
	void* m_frameReleaseUserData = nullptr;

	// =====================================================================
	// 콜백 / 설정
	// =====================================================================
	alignas(64) EncodedPacketCallback m_encodedPacketCallback = nullptr;
	void* m_encodedPacketCallbackUserData = nullptr;
	ErrorCallback m_errorCallback = nullptr;
	void* m_errorCallbackUserData = nullptr;
	SRWLOCK m_callbackLock = SRWLOCK_INIT;

	// 앱이 준 설정 원본. Reconfigure 가 [init] 필드 변경을 감지하는 기준이 된다.
	NvEncConfig m_userConfig = {};
	mutable SRWLOCK m_configLock = SRWLOCK_INIT;
};

