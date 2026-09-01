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
class ID3D11ImmediateContextGate;
class D3D11VideoProcessorNV12;
class EncodeCompletionThread;

enum class NvEncPacketStatus : uint8_t
{
	Error = 0,
	NotReady,
	PacketReady,
};

// ProcessOneOutput 의 결과.
// NotReady  : 아직 완료되지 않음. 슬롯을 그대로 유지한다.
// FrameLost : 프레임 1장을 버렸지만 슬롯은 회수했다. 파이프라인은 계속 돈다.
// Fatal     : 슬롯을 안전하게 회수할 수 없다. 이미 faulted 상태로 진입해 있다.
enum class NvEncOutputResult : uint8_t
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
	uint64_t timeStamp = 0;
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

	D3D11NvEncoder_Impl() = default;
	~D3D11NvEncoder_Impl();

	D3D11NvEncoder_Impl(const D3D11NvEncoder_Impl&) = delete;
	D3D11NvEncoder_Impl& operator=(const D3D11NvEncoder_Impl&) = delete;

	bool Initialize(
		ID3D11Device* device,
		const NvEncConfig& config,
		ID3D11ImmediateContextGate* contextGate);
	NvEncReconfigureResult Reconfigure(const NvEncConfig& config, bool forceIdr);
	void GetConfig(NvEncConfig& config) const;
	void Destroy();

	void SetEncodedPacketCallback(EncodedPacketCallback callback, void* userData);
	void SetErrorCallback(ErrorCallback callback, void* userData);
	bool PrepareFrameForEncode(ID3D11Texture2D* bgraTexture);
	void RequestKeyFrame();
	bool CanSubmitFrame() const;
	bool SubmitFrame(uint64_t frameId);
	uint32_t GetPendingFrameCount() const;
	bool WaitForPendingFrames(uint32_t timeoutMilliseconds) const;
	bool IsAsyncPipelineEnabled() const;
	bool DoEncode(NvEncPacket& encodeResultPacket);

	bool IsFaulted() const;
	void GetStats(NvEncStats& stats) const;
	void DebugFailNextOutputs(uint32_t count);

private:
	// 게이트를 획득한 상태에서 호출된다. 내부에서 게이트를 다시 잡아서는 안 된다.
	bool InitializeEncoderResources();

	bool LoadNvEncApi();
	bool OpenEncodeSession();

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
	bool InitializeSyncEvents();
	void DestroySyncEvents();

	bool InitializeAsyncEvent();
	void DestroyAsyncEvent();

	bool InitializeMappedInputBuffers();
	void DestroyMappedInputBuffers();

	bool InitializeBitstreamBuffers();
	void DestroyBitstreamBuffers();

	bool InitializeRegisteredResources();
	void DestroyRegisteredResources();

	bool InitializeD3D11InputBuffers();
	void DestroyD3D11InputBuffers();

	bool InitializeBGRAtoNV12Converter();
	void DestroyBGRAtoNV12Converter();

	bool InitializePacketBuffers();
	void DestroyPacketBuffers();
	void ReleasePacketBuffer(NvEncPacketBuffer& frame);
	bool InitializePendingFrames();
	void DestroyPendingFrames();
	bool InitializeEncodeCompletionThread();
	void StopEncodeCompletionThread();
	NvEncOutputResult ProcessOneOutput(bool block, bool invokeCallback, NvEncPacket* outPacket = nullptr);
	void ClearPendingFrame(uint32_t slot);
	void AbortPendingFrames();
	void EnterFaultedState(NvEncErrorCode errorCode);
	bool ConsumeDebugOutputFailure();
	void SignalAllSlotsFree();
	void InvokeEncodedPacketCallback(const NvEncPacket& packet);
	void InvokeErrorCallback(NvEncErrorCode errorCode);

	bool RegisterResource(void* buffer, NV_ENC_INPUT_RESOURCE_TYPE eResourceType,
		uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT eBufferFormat, NV_ENC_BUFFER_USAGE eBufferUsage,
		NV_ENC_REGISTERED_PTR& registeredResource);

	bool RegisterInputResources(void** inputFrames, uint32_t inputFrameCount, NV_ENC_INPUT_RESOURCE_TYPE eResourceType,
		uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT eBufferFormat);

	bool SetNV12OutputTexture(ID3D11Texture2D** textures, uint32_t bufferCount);
	bool SetBGRAInputTexture(ID3D11Texture2D** textures, uint32_t bufferCount);

	uint32_t GetInputSlotIndex() const;
	uint32_t GetOutputSlotIndex() const;

	bool MapInputResource(uint32_t slot);
	bool UnmapInputResource(uint32_t slot);

	bool EncodePicture(uint32_t slot);
	NvEncPacketStatus WaitForEncodeCompletion(uint32_t slot, bool block);
	bool ReadEncodedBitstream(uint32_t slot, NvEncPacket& packet);
	bool Flush();

	int32_t GetCapabilityValue(GUID guidCodec, NV_ENC_CAPS capsToQuery);
	uint32_t GetEncodeWidth() const;
	uint32_t GetEncodeHeight() const;
	uint32_t GetMaxEncodeWidth() const;
	uint32_t GetMaxEncodeHeight() const;
	NV_ENC_BUFFER_FORMAT GetPixelFormat() const;
	DXGI_FORMAT GetD3D11Format(NV_ENC_BUFFER_FORMAT eBufferFormat) const;
	HANDLE GetCompletionEvent(uint32_t slot);

private:
	ID3D11Device* m_D3D11Device = nullptr;
	ID3D11DeviceContext* m_D3D11Context = nullptr;
	ID3D11ImmediateContextGate* m_contextGate = nullptr;

	void* m_encoderHandle = nullptr;
	NV_ENCODE_API_FUNCTION_LIST m_nvenc = {};

	D3D11VideoProcessorNV12* m_converter = nullptr;

	NV_ENC_INITIALIZE_PARAMS m_initParameters = {};
	NV_ENC_CONFIG m_config = {};

	HANDLE* m_slotCompletionEvents = nullptr;
	HANDLE m_eosCompletionEvent = nullptr;

	// Initialize 에서 만들고 Destroy 끝에서 닫는다. 엔코더 수명 전체에 걸쳐 유효.
	HANDLE m_allSlotsFreeEvent = nullptr;
	HANDLE m_frameSubmittedEvent = nullptr;

	EncodeCompletionThread* m_encodeCompletionThread = nullptr;

	uint32_t m_encodeBufferCount = 1;
	NvEncPacketBuffer* m_packetBuffers = nullptr;
	NvEncPendingFrame* m_pendingFrames = nullptr;
	NV_ENC_REGISTERED_PTR* m_registeredResources = nullptr;
	NV_ENC_INPUT_PTR* m_mappedInputBuffers = nullptr;
	NV_ENC_OUTPUT_PTR* m_bitstreamBuffers = nullptr;

	ID3D11Texture2D** m_bgraTextures = nullptr;
	ID3D11Texture2D** m_nv12Textures = nullptr;

	uint64_t m_timeStamp = 0;
	uint32_t m_inputSequence = 0;
	uint32_t m_outputSequence = 0;
	alignas(4) volatile LONG m_pendingFrameCount = 0;
	alignas(4) volatile LONG m_forceKeyFrame = FALSE;
	alignas(4) volatile LONG m_acceptFrames = FALSE;
	alignas(4) volatile LONG m_faulted = FALSE;
	alignas(4) volatile LONG m_debugFailOutputCount = 0;
	alignas(8) volatile LONG64 m_submittedFrameCount = 0;
	alignas(8) volatile LONG64 m_completedFrameCount = 0;
	alignas(8) volatile LONG64 m_lostFrameCount = 0;
	bool m_asyncPipelineEnabled = true;
	EncodedPacketCallback m_encodedPacketCallback = nullptr;
	void* m_encodedPacketCallbackUserData = nullptr;
	ErrorCallback m_errorCallback = nullptr;
	void* m_errorCallbackUserData = nullptr;
	SRWLOCK m_callbackLock = SRWLOCK_INIT;

	uint32_t m_width = 0;
	uint32_t m_height = 0;

	// 앱이 준 설정 원본. Reconfigure 가 [init] 필드 변경을 감지하는 기준이 된다.
	NvEncConfig m_userConfig = {};
	mutable SRWLOCK m_configLock = SRWLOCK_INIT;
};
