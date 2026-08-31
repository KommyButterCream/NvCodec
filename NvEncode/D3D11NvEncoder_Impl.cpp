#include "pch.h"
#include "D3D11NvEncoder_Impl.h"
#include "../../D3D11EngineInterface/ID3D11ImmediateContextGate.h"
#include "D3D11VideoProcessorNV12.h"
#include "EncodeCompletionThread.h"

#include <new> // for std::nothrow
#include <stdio.h> // for printf_s, fopen_s, fwrite

namespace
{
	// async 파이프라인이 성립하는 최소 버퍼 수량.
	constexpr uint32_t kMinEncodeBufferCount = 2U;

	inline bool IsPowerOfTwo(uint32_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	inline uint32_t WrapRingIndex(uint32_t sequence, uint32_t bufferCount)
	{
		return sequence & (bufferCount - 1);
	}

	inline bool CheckNvEncodeAPICall(
		NVENCSTATUS errorCode,
		const char* expr,
		const char* func,
		const char* file,
		int32_t line)
	{
		if (errorCode == NV_ENC_SUCCESS)
			return true;

		printf_s("[NVENC ERROR]\n");
		printf_s("  API   : %s\n", expr);
		printf_s("  Code  : %d\n", errorCode);
		printf_s("  Where : %s (%s:%d)\n\n", func, file, line);
		return false;
	}

#define NVENC_API_CALL(call) \
    CheckNvEncodeAPICall((call), #call, __FUNCTION__, __FILE__, __LINE__)

}

D3D11NvEncoder_Impl::~D3D11NvEncoder_Impl()
{
	Destroy();
}

bool D3D11NvEncoder_Impl::Initialize(
	ID3D11Device* device,
	uint32_t width,
	uint32_t height,
	uint32_t encodeBufferCount,
	ID3D11ImmediateContextGate* contextGate,
	bool enableAsyncPipeline)
{
	// encodeBufferCount 가 1 이면 제출 -> 대기 -> 완료가 완전히 직렬화되어
	// async 파이프라인의 의미가 사라진다. 최소 2 를 요구한다.
	if (!device || width == 0 || height == 0 ||
		encodeBufferCount < kMinEncodeBufferCount || !IsPowerOfTwo(encodeBufferCount))
	{
		printf_s("[NVENC ERROR] Invalid encoder parameters. width=%u height=%u encodeBufferCount=%u"
			" (encodeBufferCount must be a power of two and at least %u)\n",
			width, height, encodeBufferCount, kMinEncodeBufferCount);
		return false;
	}

	Destroy();

	// 건네 받은 D3D11 Device, Context 포인터의 참조 횟수 증가
	// Destroy 시점에 Release 호출 필요
	m_D3D11Device = device;
	m_D3D11Device->AddRef();
	m_D3D11Device->GetImmediateContext(&m_D3D11Context);
	m_contextGate = contextGate;

	m_width = width;
	m_height = height;
	m_encodeBufferCount = encodeBufferCount;
	m_asyncPipelineEnabled = enableAsyncPipeline;

	m_timeStamp = 0;
	m_inputSequence = 0;
	m_outputSequence = 0;
	::InterlockedExchange(&m_pendingFrameCount, 0);
	::InterlockedExchange(&m_forceKeyFrame, FALSE);
	::InterlockedExchange(&m_acceptFrames, FALSE);
	::InterlockedExchange(&m_faulted, FALSE);
	::InterlockedExchange(&m_debugFailOutputCount, 0);
	::InterlockedExchange64(&m_submittedFrameCount, 0);
	::InterlockedExchange64(&m_completedFrameCount, 0);
	::InterlockedExchange64(&m_lostFrameCount, 0);


	if (!InitializeSyncEvents())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializeSyncEvents.\n");
		DestroySyncEvents();
		SafeRelease(m_D3D11Context);
		SafeRelease(m_D3D11Device);
		m_contextGate = nullptr;
		return false;
	}

	// NVENC 리소스 등록/세션 생성은 D3D11 디바이스와 컨텍스트를 내부에서 만진다.
	// 초기화 시퀀스 전체를 한 번의 게이트 획득으로 원자적으로 처리한다.
	{
		D3D11ImmediateContextGuard contextGuard(m_contextGate);
		if (!InitializeEncoderResources())
		{
			DestroySyncEvents();
			SafeRelease(m_D3D11Context);
			SafeRelease(m_D3D11Device);
			m_contextGate = nullptr;
			return false;
		}
	}

	// 완료 스레드는 반드시 게이트 밖에서 시작한다.
	// 게이트를 잡은 채로 띄우면 그 스레드가 게이트를 요구하는 순간 데드락이다.
	if (m_asyncPipelineEnabled && !InitializeEncodeCompletionThread())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializeEncodeCompletionThread.\n");
		Destroy();
		return false;
	}

	::InterlockedExchange(&m_acceptFrames, TRUE);
	return true;
}

// 게이트를 획득한 상태에서 호출된다. 내부에서 게이트를 다시 잡아서는 안 된다.
bool D3D11NvEncoder_Impl::InitializeEncoderResources()
{
	// 단계별 초기화 진행
	// 실패 시 goto 로 정리 순서 보장

	if (!LoadNvEncApi())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: LoadNvEncApi.\n");
		goto fail_device;
	}

	if (!OpenEncodeSession())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: OpenEncodeSession.\n");
		goto fail_device;
	}

	if (!InitializeEncoder())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializeEncoder.\n");
		goto fail_converter;
	}

	if (!InitializeBGRAtoNV12Converter())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializeBGRAtoNV12Converter.\n");
		goto fail_converter;
	}

	if (!InitializeAsyncEvent())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializeAsyncEvent.\n");
		goto fail_async_event;
	}

	if (!InitializeBitstreamBuffers())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializeBitstreamBuffers.\n");
		goto fail_bitstream;
	}

	if (!InitializeRegisteredResources())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializeRegisteredResources.\n");
		goto fail_registered_resources;
	}

	if (!InitializeD3D11InputBuffers())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializeD3D11InputBuffers.\n");
		goto fail_input_buffers;
	}

	if (!InitializeMappedInputBuffers())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializeMappedInputBuffers.\n");
		goto fail_mapped_inputs;
	}

	if (!InitializePacketBuffers())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializePacketBuffers.\n");
		goto fail_output_frames;
	}

	if (!InitializePendingFrames())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: InitializePendingFrames.\n");
		goto fail_in_flight_frames;
	}

	// 완료 스레드는 호출자가 게이트를 해제한 뒤에 시작한다.
	return true;

fail_in_flight_frames:
	DestroyPacketBuffers();
fail_output_frames:
	DestroyMappedInputBuffers();
fail_mapped_inputs:
	DestroyD3D11InputBuffers();
fail_input_buffers:
	DestroyRegisteredResources();
fail_registered_resources:
	DestroyBitstreamBuffers();
fail_bitstream:
	DestroyAsyncEvent();
fail_async_event:
	DestroyBGRAtoNV12Converter();
fail_converter:
	DestroyEncoder();
	return false;
fail_device:
	DestroyEncoder();
	return false;
}

void D3D11NvEncoder_Impl::Destroy()
{
	::InterlockedExchange(&m_acceptFrames, FALSE);

	if (m_encodeCompletionThread)
	{
		StopEncodeCompletionThread();
	}
	else if (!Flush() && !IsFaulted())
	{
		printf_s("[NVENC ERROR] Flush failed during Destroy().\n");
	}

	SetEncodedPacketCallback(nullptr, nullptr);
	SetErrorCallback(nullptr, nullptr);

	// Unregister / Unmap / DestroyEncoder 는 D3D11 리소스를 만진다.
	// 앱의 렌더 스레드가 계속 돌고 있을 수 있으므로 게이트 안에서 처리한다.
	// 완료 스레드는 위에서 이미 정지했다(게이트 밖에서 정지시켜야 한다.
	// Flush 와 완료 스레드가 내부에서 게이트를 잡기 때문).
	{
		D3D11ImmediateContextGuard contextGuard(m_contextGate);

		DestroyPendingFrames();
		DestroyPacketBuffers();
		DestroyMappedInputBuffers();
		DestroyRegisteredResources();
		DestroyD3D11InputBuffers();
		DestroyBitstreamBuffers();
		DestroyAsyncEvent();
		DestroyBGRAtoNV12Converter();
		DestroyEncoder();
	}

	// 동기 이벤트는 위의 모든 스레드가 정지한 뒤에 닫는다.
	DestroySyncEvents();

	SafeRelease(m_D3D11Context);
	SafeRelease(m_D3D11Device);
	m_contextGate = nullptr;
}

bool D3D11NvEncoder_Impl::InitializeSyncEvents()
{
	// all-slots-free 는 manual reset, 초기 상태 signaled(= pending 프레임 없음).
	// frame-submitted 는 auto reset. 완료 스레드를 깨우는 용도.
	m_allSlotsFreeEvent = ::CreateEvent(nullptr, TRUE, TRUE, nullptr);
	m_frameSubmittedEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);

	if (!m_allSlotsFreeEvent || !m_frameSubmittedEvent)
	{
		DestroySyncEvents();
		return false;
	}

	return true;
}

void D3D11NvEncoder_Impl::DestroySyncEvents()
{
	if (m_allSlotsFreeEvent)
	{
		::CloseHandle(m_allSlotsFreeEvent);
		m_allSlotsFreeEvent = nullptr;
	}

	if (m_frameSubmittedEvent)
	{
		::CloseHandle(m_frameSubmittedEvent);
		m_frameSubmittedEvent = nullptr;
	}
}

void D3D11NvEncoder_Impl::SetEncodedPacketCallback(EncodedPacketCallback callback, void* userData)
{
	::AcquireSRWLockExclusive(&m_callbackLock);
	m_encodedPacketCallback = callback;
	m_encodedPacketCallbackUserData = userData;
	::ReleaseSRWLockExclusive(&m_callbackLock);
}

void D3D11NvEncoder_Impl::SetErrorCallback(ErrorCallback callback, void* userData)
{
	::AcquireSRWLockExclusive(&m_callbackLock);
	m_errorCallback = callback;
	m_errorCallbackUserData = userData;
	::ReleaseSRWLockExclusive(&m_callbackLock);
}

bool D3D11NvEncoder_Impl::PrepareFrameForEncode(ID3D11Texture2D* bgraTexture)
{
	// Encode 를 수행하기 위한 텍스쳐를 BGRA Texture Pool 에 복사한다.
	if (!m_encoderHandle)
	{
		printf_s("[NVENC ERROR] Encoder handle is not initialized.\n");
		return false;
	}
	if (!CanSubmitFrame())
	{
		return false;
	}

	if (!m_D3D11Context)
	{
		printf_s("[D3D11 Context ERROR] D3D11 Context is not initialized.\n");
		return false;
	}

	const uint32_t inputSlot = GetInputSlotIndex();

	ID3D11Texture2D* dstTexture = m_bgraTextures[inputSlot];

	// GPU -> GPU copy
	{
		D3D11ImmediateContextGuard contextGuard(m_contextGate);
		m_D3D11Context->CopyResource(dstTexture, bgraTexture);
	}

	return true;
}

void D3D11NvEncoder_Impl::RequestKeyFrame()
{
	::InterlockedExchange(&m_forceKeyFrame, TRUE);
}

bool D3D11NvEncoder_Impl::CanSubmitFrame() const
{
	return m_encoderHandle && m_pendingFrames &&
		::InterlockedCompareExchange(const_cast<volatile LONG*>(&m_acceptFrames), TRUE, TRUE) == TRUE &&
		GetPendingFrameCount() < m_encodeBufferCount;
}

bool D3D11NvEncoder_Impl::SubmitFrame(uint64_t frameId)
{
	if (!CanSubmitFrame())
		return false;

	const uint32_t inputSlot = GetInputSlotIndex();
	NvEncPendingFrame& pendingFrame = m_pendingFrames[inputSlot];
	if (::InterlockedCompareExchange(&pendingFrame.submitted, TRUE, TRUE) == TRUE)
		return false;

	if (!m_converter || !m_converter->Convert(inputSlot))
		return false;

	if (!MapInputResource(inputSlot))
		return false;

	if (!EncodePicture(inputSlot))
	{
		UnmapInputResource(inputSlot);
		return false;
	}

	pendingFrame.frameId = frameId;
	::InterlockedExchange(&pendingFrame.submitted, TRUE);
	m_inputSequence++;
	::InterlockedIncrement(&m_pendingFrameCount);
	::InterlockedIncrement64(&m_submittedFrameCount);

	// pending 을 올린 "뒤에" 리셋해야 한다. 올리기 전 값으로 판단하면
	// 그 사이에 완료 스레드가 pending 을 0 으로 만들며 SetEvent 한 것을
	// 되돌리지 못해 pending > 0 인데 SET 인 상태가 남는다.
	// 리셋 직후 다시 확인해서 그 사이에 드레인이 끝났으면 되돌린다.
	if (m_allSlotsFreeEvent)
	{
		::ResetEvent(m_allSlotsFreeEvent);
		if (GetPendingFrameCount() == 0)
			::SetEvent(m_allSlotsFreeEvent);
	}

	// 완료 스레드 객체 포인터를 만지지 않는다. 그 포인터는 StopEncodeCompletionThread
	// 가 지우므로 여기서 역참조하면 UAF 창이 생긴다. 이벤트는 엔코더 수명 전체 유효.
	if (m_frameSubmittedEvent)
		::SetEvent(m_frameSubmittedEvent);
	return true;
}

uint32_t D3D11NvEncoder_Impl::GetPendingFrameCount() const
{
	return static_cast<uint32_t>(::InterlockedCompareExchange(
		const_cast<volatile LONG*>(&m_pendingFrameCount), 0, 0));
}

bool D3D11NvEncoder_Impl::WaitForPendingFrames(uint32_t timeoutMilliseconds) const
{
	if (GetPendingFrameCount() == 0)
		return true;
	if (!m_allSlotsFreeEvent)
		return false;
	if (::WaitForSingleObject(m_allSlotsFreeEvent, timeoutMilliseconds) != WAIT_OBJECT_0)
		return false;
	return GetPendingFrameCount() == 0;
}

bool D3D11NvEncoder_Impl::IsAsyncPipelineEnabled() const
{
	return m_asyncPipelineEnabled;
}

bool D3D11NvEncoder_Impl::DoEncode(NvEncPacket& encodeResultPacket)
{
	if (m_asyncPipelineEnabled)
		return false;

	if (!SubmitFrame(0))
		return false;

	// 슬롯 회수와 실패 복구는 ProcessOneOutput 에 한 곳으로 모아둔다.
	return ProcessOneOutput(true, false, &encodeResultPacket) == NvEncOutputResult::Completed;
}

bool D3D11NvEncoder_Impl::IsFaulted() const
{
	return ::InterlockedCompareExchange(const_cast<volatile LONG*>(&m_faulted), TRUE, TRUE) == TRUE;
}

void D3D11NvEncoder_Impl::GetStats(NvEncStats& stats) const
{
	stats.submittedFrames = static_cast<uint64_t>(
		::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_submittedFrameCount), 0, 0));
	stats.completedFrames = static_cast<uint64_t>(
		::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_completedFrameCount), 0, 0));
	stats.lostFrames = static_cast<uint64_t>(
		::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_lostFrameCount), 0, 0));
	stats.pendingFrames = GetPendingFrameCount();
	stats.faulted = IsFaulted();
}

void D3D11NvEncoder_Impl::DebugFailNextOutputs(uint32_t count)
{
	::InterlockedExchange(&m_debugFailOutputCount, static_cast<LONG>(count));
}

bool D3D11NvEncoder_Impl::ConsumeDebugOutputFailure()
{
	// 테스트 훅. 남은 횟수가 있으면 하나 소비하고 실패를 지시한다.
	LONG remaining = ::InterlockedCompareExchange(&m_debugFailOutputCount, 0, 0);
	while (remaining > 0)
	{
		const LONG previous = ::InterlockedCompareExchange(&m_debugFailOutputCount, remaining - 1, remaining);
		if (previous == remaining)
			return true;
		remaining = previous;
	}

	return false;
}

bool D3D11NvEncoder_Impl::LoadNvEncApi()
{
	// NVENC API Function Table 을 로드한다.
	uint32_t version = 0;
	uint32_t currentVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
	if (!NVENC_API_CALL(NvEncodeAPIGetMaxSupportedVersion(&version)))
		return false;
	if (currentVersion > version)
	{
		printf_s("[NVENC ERROR] Current Driver Version does not support this NvEncodeAPI version.\n");
		return false;
	}

	m_nvenc.version = NV_ENCODE_API_FUNCTION_LIST_VER;

	return NVENC_API_CALL(NvEncodeAPICreateInstance(&m_nvenc));
}

bool D3D11NvEncoder_Impl::OpenEncodeSession()
{
	// D3D11 Texture 를 사용하므로
	// D3D11 Device 기반 Encoder Session 생성

	if (!m_nvenc.nvEncOpenEncodeSession)
	{
		printf_s("[NVENC ERROR] EncodeAPI not found.\n");
		return false;
	}

	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { };
	sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
	sessionParams.device = m_D3D11Device;
	sessionParams.deviceType = NV_ENC_DEVICE_TYPE::NV_ENC_DEVICE_TYPE_DIRECTX;
	sessionParams.apiVersion = NVENCAPI_VERSION;
	void* hEncoder = nullptr;
	if (!NVENC_API_CALL(m_nvenc.nvEncOpenEncodeSessionEx(&sessionParams, &hEncoder)))
		return false;

	m_encoderHandle = hEncoder;
	return true;
}

bool D3D11NvEncoder_Impl::InitializeEncoder()
{
	// Encoding 파라메터를 설정 하고 NVENC Encoder 를 생성한다.
	if (!m_encoderHandle)
	{
		printf_s("[NVENC ERROR] Encoder handle is not initialized.\n");
		return false;
	}

	memset(&m_initParameters, 0, sizeof(m_initParameters));
	memset(&m_config, 0, sizeof(m_config));

	// 실시간 스트리밍에 적합한 Low Latency Profile 기본 설정값을 가져온다.
	NV_ENC_PRESET_CONFIG presetConfig = {};
	presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
	presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

	if (!NVENC_API_CALL(m_nvenc.nvEncGetEncodePresetConfigEx(
		m_encoderHandle,
		NV_ENC_CODEC_H264_GUID,
		NV_ENC_PRESET_P3_GUID,
		NV_ENC_TUNING_INFO::NV_ENC_TUNING_INFO_LOW_LATENCY,
		&presetConfig)))
	{
		return false;
	}

	// 프리셋 설정값을 복사
	memcpy(&m_config, &presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
	m_config.version = NV_ENC_CONFIG_VER;

	// 여기서부터는 적절하게 파라메터를 수정한다.
	// 아래에서 설정하는 파라메터는 실시간 스트리밍에 적합하도록 설정

	// RC
	m_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_MODE::NV_ENC_PARAMS_RC_CBR;
	m_config.rcParams.averageBitRate = 5000000;
	m_config.rcParams.maxBitRate = 5000000;

	// GOP
	m_config.gopLength = 30;
	m_config.frameIntervalP = 1;

	// H.264 Config
	m_config.encodeCodecConfig.h264Config.idrPeriod = 60;
	m_config.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
	m_config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
	m_config.encodeCodecConfig.h264Config.h264VUIParameters.videoFullRangeFlag = 1;
	m_config.encodeCodecConfig.h264Config.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
	m_config.encodeCodecConfig.h264Config.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
	m_config.encodeCodecConfig.h264Config.h264VUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
	m_config.encodeCodecConfig.h264Config.h264VUIParameters.videoSignalTypePresentFlag = 1;
	m_config.encodeCodecConfig.h264Config.h264VUIParameters.colourDescriptionPresentFlag = 1;

	// Encoder Initialize Parameters
	m_initParameters.version = NV_ENC_INITIALIZE_PARAMS_VER;
	m_initParameters.encodeConfig = &m_config;
	m_initParameters.encodeConfig->version = NV_ENC_CONFIG_VER;


	// H.264 코덱을 사용, AV1 이나 기타 코덱은 추후 개발 예정
	m_initParameters.encodeGUID = NV_ENC_CODEC_H264_GUID;
	m_initParameters.presetGUID = NV_ENC_PRESET_P3_GUID;
	m_initParameters.encodeWidth = m_width;
	m_initParameters.encodeHeight = m_height;
	m_initParameters.darWidth = m_width;
	m_initParameters.darHeight = m_height;
	m_initParameters.frameRateNum = 30;
	m_initParameters.frameRateDen = 1;
	m_initParameters.enablePTD = 1;
	m_initParameters.reportSliceOffsets = 0;
	m_initParameters.enableSubFrameWrite = 0;
	m_initParameters.maxEncodeWidth = m_width;
	m_initParameters.maxEncodeHeight = m_height;
	m_initParameters.enableMEOnlyMode = false;
	m_initParameters.enableOutputInVidmem = false;
	m_initParameters.enableEncodeAsync = m_asyncPipelineEnabled
		? GetCapabilityValue(NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS::NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT)
		: 0U;
	m_initParameters.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
	m_initParameters.tuningInfo = NV_ENC_TUNING_INFO::NV_ENC_TUNING_INFO_LOW_LATENCY;

	// 위에서 설정된 파라메터로 NVENC Encoder 생성
	return NVENC_API_CALL(m_nvenc.nvEncInitializeEncoder(m_encoderHandle, &m_initParameters));
}

void D3D11NvEncoder_Impl::DestroyEncoder()
{
	// NVENC Encoder 리소스를 해제한다.
	if (!m_encoderHandle)
	{
		return;
	}

	NVENC_API_CALL(m_nvenc.nvEncDestroyEncoder(m_encoderHandle));
	m_encoderHandle = nullptr;
}

bool D3D11NvEncoder_Impl::InitializeAsyncEvent()
{
	// 비동기로 처리되는 NVENC Encode 완료 이벤트를 통지 받기 위한
	// 이벤트를 생성 후 NVENC 에 Register 한다.

	if (!m_encoderHandle)
	{
		printf_s("[NVENC ERROR] Encoder handle is not initialized.\n");
		return false;
	}

	if (m_initParameters.enableEncodeAsync == 0U)
		return true;

	// Async Event Create & Register
	m_slotCompletionEvents = new (std::nothrow) HANDLE[m_encodeBufferCount]{};
	if (!m_slotCompletionEvents)
		return false;

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		// 이벤트 생성
		m_slotCompletionEvents[i] = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!m_slotCompletionEvents[i])
		{
			DestroyAsyncEvent();
			return false;
		}

		// NVENC 등록
		NV_ENC_EVENT_PARAMS eventParams = { NV_ENC_EVENT_PARAMS_VER };
		eventParams.completionEvent = m_slotCompletionEvents[i];
		if (!NVENC_API_CALL(m_nvenc.nvEncRegisterAsyncEvent(m_encoderHandle, &eventParams)))
		{
			DestroyAsyncEvent();
			return false;
		}
	}

	m_eosCompletionEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!m_eosCompletionEvent)
	{
		DestroyAsyncEvent();
		return false;
	}

	NV_ENC_EVENT_PARAMS eosEventParams = { NV_ENC_EVENT_PARAMS_VER };
	eosEventParams.completionEvent = m_eosCompletionEvent;
	if (!NVENC_API_CALL(m_nvenc.nvEncRegisterAsyncEvent(m_encoderHandle, &eosEventParams)))
	{
		::CloseHandle(m_eosCompletionEvent);
		m_eosCompletionEvent = nullptr;
		DestroyAsyncEvent();
		return false;
	}

	return true;
}

void D3D11NvEncoder_Impl::DestroyAsyncEvent()
{
	// 종료 시점에 등록 되어있던 이벤트를 Unregister 하고
	// 이벤트 핸들을 삭제한다.
	//
	// 엔코더 핸들이 이미 사라진 경우에도 통째로 return 하면 안 된다.
	// Unregister 만 건너뛰고 커널 핸들과 배열은 반드시 해제해야 한다.
	const bool canCallNvEnc = (m_encoderHandle != nullptr);

	if (m_slotCompletionEvents)
	{
		for (uint32_t i = 0; i < m_encodeBufferCount; i++)
		{
			HANDLE& completionEvent = m_slotCompletionEvents[i];
			if (completionEvent)
			{
				if (canCallNvEnc)
				{
					NV_ENC_EVENT_PARAMS eventParams = { NV_ENC_EVENT_PARAMS_VER };
					eventParams.completionEvent = completionEvent;
					NVENC_API_CALL(m_nvenc.nvEncUnregisterAsyncEvent(m_encoderHandle, &eventParams));
				}

				::CloseHandle(completionEvent);
				completionEvent = nullptr;
			}
		}

		delete[] m_slotCompletionEvents;
		m_slotCompletionEvents = nullptr;
	}

	if (m_eosCompletionEvent)
	{
		if (canCallNvEnc)
		{
			NV_ENC_EVENT_PARAMS eventParams = { NV_ENC_EVENT_PARAMS_VER };
			eventParams.completionEvent = m_eosCompletionEvent;
			NVENC_API_CALL(m_nvenc.nvEncUnregisterAsyncEvent(m_encoderHandle, &eventParams));
		}
		::CloseHandle(m_eosCompletionEvent);
		m_eosCompletionEvent = nullptr;
	}
}

bool D3D11NvEncoder_Impl::InitializeMappedInputBuffers()
{
	// NVENC Encoding 을 위한 Input Buffer 를 미리 생성한다.
	// Encode 수행 함수를 호출할때 NV_ENC_INPUT_PTR 타입 필요.
	m_mappedInputBuffers = new (std::nothrow) NV_ENC_INPUT_PTR[m_encodeBufferCount]{};
	return (m_mappedInputBuffers != nullptr);
}

void D3D11NvEncoder_Impl::DestroyMappedInputBuffers()
{
	// Input Buffer 해제
	// Map 되어 있는 리소스가 있다면 해제 해준다.
	// 엔코더 핸들이 없어도 배열은 해제한다.
	const bool canCallNvEnc = (m_encoderHandle != nullptr);

	if (!m_mappedInputBuffers)
	{
		return;
	}

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		NV_ENC_INPUT_PTR& mappedInputBuffer = m_mappedInputBuffers[i];
		if (mappedInputBuffer)
		{
			if (canCallNvEnc)
				NVENC_API_CALL(m_nvenc.nvEncUnmapInputResource(m_encoderHandle, mappedInputBuffer));
			mappedInputBuffer = nullptr;
		}
	}

	delete[] m_mappedInputBuffers;
	m_mappedInputBuffers = nullptr;
}

bool D3D11NvEncoder_Impl::InitializeBitstreamBuffers()
{
	// NVENC Encode 결과를 저장하기 위한 Output Buffer 생성
	if (!m_encoderHandle)
	{
		printf_s("[NVENC ERROR] Encoder handle is not initialized.\n");
		return false;
	}

	m_bitstreamBuffers = new (std::nothrow) NV_ENC_OUTPUT_PTR[m_encodeBufferCount]{};
	if (!m_bitstreamBuffers)
		return false;

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		// Output Buffer 생성
		NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamBufferParams = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
		if (!NVENC_API_CALL(m_nvenc.nvEncCreateBitstreamBuffer(m_encoderHandle, &bitstreamBufferParams)))
		{
			DestroyBitstreamBuffers();
			return false;
		}
		m_bitstreamBuffers[i] = bitstreamBufferParams.bitstreamBuffer;
	}

	return true;
}

void D3D11NvEncoder_Impl::DestroyBitstreamBuffers()
{
	// Output Buffer 리소스 해제
	// 엔코더 핸들이 없어도 배열은 해제한다.
	const bool canCallNvEnc = (m_encoderHandle != nullptr);

	if (!m_bitstreamBuffers)
	{
		return;
	}

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		NV_ENC_OUTPUT_PTR& bitstreamBuffer = m_bitstreamBuffers[i];
		if (bitstreamBuffer)
		{
			if (canCallNvEnc)
				NVENC_API_CALL(m_nvenc.nvEncDestroyBitstreamBuffer(m_encoderHandle, bitstreamBuffer));
			bitstreamBuffer = nullptr;
		}
	}

	delete[] m_bitstreamBuffers;
	m_bitstreamBuffers = nullptr;
}

bool D3D11NvEncoder_Impl::InitializeRegisteredResources()
{
	// NVENC 내부에서 관리하는 Registered 리소스 핸들을 저장할 공간을 만든다.
	// NVENC 가 접근하기 위해서는 Encode 호출 전 사전에 미리 Registered 되어야 한다.
	m_registeredResources = new (std::nothrow) NV_ENC_REGISTERED_PTR[m_encodeBufferCount]{};
	return (m_registeredResources != nullptr);
}

void D3D11NvEncoder_Impl::DestroyRegisteredResources()
{
	// NVENC 내부에서 관리하는 Registered 리소스 해제
	// 엔코더 핸들이 없어도 배열은 해제한다.
	const bool canCallNvEnc = (m_encoderHandle != nullptr);

	if (!m_registeredResources)
	{
		return;
	}

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		NV_ENC_REGISTERED_PTR& registeredResource = m_registeredResources[i];
		if (registeredResource)
		{
			if (canCallNvEnc)
				NVENC_API_CALL(m_nvenc.nvEncUnregisterResource(m_encoderHandle, registeredResource));
			registeredResource = nullptr;
		}
	}

	delete[] m_registeredResources;
	m_registeredResources = nullptr;
}

bool D3D11NvEncoder_Impl::InitializeD3D11InputBuffers()
{
	// NVENC 는 NV12 와 같은 특수 타입의 데이터만 Input 으로 받을 수 있다.
	// 버퍼풀 수량 만큼의 BGRA, NV12 D3D11 Texture2D 를 생성 하고
	// BGRA 버퍼는 Encoding 전 BGRA -> NV12 변환을 위한 전처리 버퍼로 사용하기 위해 설정하고
	// NV12 버퍼는 BGRA -> NV12 변환이 끝난 결과를 저장하며 NVENC 에게 
	// Input 으로 사용하겠다는 의도로 RegisterInputResources 한다.
	// 또한 BGRA -> NV12 변환은 D3D11VideoProcessorNV12 에서 수행되므로
	// D3D11VideoProcessorNV12 에 Input, Output Buffer 로 설정한다.
	// 이렇게 하여 BGRA -> NV12 변환된 텍스쳐를 그대로 Encode Input 으로 사용 가능하게 된다.

	if (!m_encoderHandle || !m_D3D11Device)
	{
		return false;
	}

	// BGRA->NV12 변환을 수행 하기 위한 Input BGRA 버퍼
	m_bgraTextures = new (std::nothrow) ID3D11Texture2D * [m_encodeBufferCount] {};
	if (!m_bgraTextures)
		return false;

	// BGRA->NV12 변환 결과를 저장하기 위한 Output NV12 버퍼
	m_nv12Textures = new (std::nothrow) ID3D11Texture2D * [m_encodeBufferCount] {};
	if (!m_nv12Textures)
	{
		// 앞서 할당한 BGRA 배열을 해제하지 않으면 누수한다.
		DestroyD3D11InputBuffers();
		return false;
	}

	HRESULT hr = S_OK;

	// BGRA 타입 D3D11Texture2D 생성
	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = GetMaxEncodeWidth();
		texDesc.Height = GetMaxEncodeHeight();
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		hr = m_D3D11Device->CreateTexture2D(&texDesc, nullptr, &m_bgraTextures[i]);
		if (FAILED(hr))
		{
			printf_s("[NVENC ERROR] Failed to create d3d11textures.\n");
			DestroyD3D11InputBuffers();
			return false;
		}
	}


	// NV12 타입 D3D11Texture2D 생성
	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = GetMaxEncodeWidth();
		desc.Height = GetMaxEncodeHeight();
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = GetD3D11Format(GetPixelFormat());
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;

		hr = m_D3D11Device->CreateTexture2D(&desc, NULL, &m_nv12Textures[i]);

		if (FAILED(hr))
		{
			printf_s("[NVENC ERROR] Failed to create d3d11textures.\n");
			DestroyD3D11InputBuffers();
			return false;
		}
	}

	// NVENC API 는 void* 기반이라 캐스팅을 위한 임시 inputFrames 버퍼 생성
	void** inputFrames = new (std::nothrow) void* [m_encodeBufferCount] {};
	if (!inputFrames)
	{
		DestroyD3D11InputBuffers();
		return false;
	}

	// D3D11Texture 의 주소만 void* 캐스팅 해서 저장
	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		inputFrames[i] = m_nv12Textures[i];
	}

	// NVENC InputResource 로 등록
	if (!RegisterInputResources(inputFrames, m_encodeBufferCount, NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX,
		GetMaxEncodeWidth(), GetMaxEncodeHeight(), GetMaxEncodeWidth(), GetPixelFormat()))
	{
		delete[] inputFrames;
		DestroyD3D11InputBuffers();
		DestroyRegisteredResources();
		InitializeRegisteredResources();
		return false;
	}

	// D3D11VideoProcessorNV12 에게 BGRA -> NV12 변환 입력을 저장하게될 버퍼로 설정
	if (!SetBGRAInputTexture(m_bgraTextures, m_encodeBufferCount))
	{
		delete[] inputFrames;
		DestroyD3D11InputBuffers();
		DestroyRegisteredResources();
		InitializeRegisteredResources();
		return false;
	}

	// D3D11VideoProcessorNV12 에게 BGRA -> NV12 변환 결과를 저장하게될 버퍼로 설정
	if (!SetNV12OutputTexture(m_nv12Textures, m_encodeBufferCount))
	{
		delete[] inputFrames;
		DestroyD3D11InputBuffers();
		DestroyRegisteredResources();
		InitializeRegisteredResources();
		return false;
	}

	delete[] inputFrames;
	return true;
}

void D3D11NvEncoder_Impl::DestroyD3D11InputBuffers()
{
	// NVENC Encode 를 위한 리소스 해제

	// 두 배열을 독립적으로 처리한다.
	// 한쪽만 null 일 때 통째로 return 하면 나머지 배열과 텍스처를 누수한다.
	if (m_bgraTextures)
	{
		for (uint32_t i = 0; i < m_encodeBufferCount; i++)
			SafeRelease(m_bgraTextures[i]);

		delete[] m_bgraTextures;
		m_bgraTextures = nullptr;
	}

	if (m_nv12Textures)
	{
		for (uint32_t i = 0; i < m_encodeBufferCount; i++)
			SafeRelease(m_nv12Textures[i]);

		delete[] m_nv12Textures;
		m_nv12Textures = nullptr;
	}
}

bool D3D11NvEncoder_Impl::InitializeBGRAtoNV12Converter()
{
	// BGRA -> NV12 변환 작업을 해주는 Converter 생성 및 초기화
	m_converter = new (std::nothrow) D3D11VideoProcessorNV12();
	if (!m_converter)
		return false;

	bool result = m_converter->Initialize(
		m_D3D11Device,
		m_D3D11Context,
		m_width,
		m_height,
		m_contextGate);
	if (!result)
	{
		delete m_converter;
		m_converter = nullptr;
	}

	return result;
}

void D3D11NvEncoder_Impl::DestroyBGRAtoNV12Converter()
{
	// BGRA -> NV12 변환 작업을 해주는 Converter 해제
	if (m_converter)
	{
		m_converter->Destroy();
		delete m_converter;
		m_converter = nullptr;
	}
}

bool D3D11NvEncoder_Impl::InitializePacketBuffers()
{
	// Encode 결과를 저장해줄 OutputFrame 생성
	// 버퍼 수량 만큼의 공간만 할당하고 실제 Encode Result 저장할 공간은
	// Bitstream 을 읽어올 때 설정한다.
	m_packetBuffers = new (std::nothrow) NvEncPacketBuffer[m_encodeBufferCount]{};
	return (m_packetBuffers != nullptr);
}

void D3D11NvEncoder_Impl::DestroyPacketBuffers()
{
	// Encode 결과를 저장해줄 OutputFrame 해제

	if (!m_packetBuffers)
	{
		return;
	}

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		ReleasePacketBuffer(m_packetBuffers[i]);
	}

	delete[] m_packetBuffers;
	m_packetBuffers = nullptr;
}

void D3D11NvEncoder_Impl::ReleasePacketBuffer(NvEncPacketBuffer& frame)
{
	// 프레임 데이터를 정리한다.
	delete[] frame.streamData;
	frame.streamData = nullptr;
	frame.streamDataSize = 0;
	frame.streamDataCapacity = 0;
	frame.pictureType = NV_ENC_PIC_TYPE_UNKNOWN;
	frame.timeStamp = 0;
	frame.isKeyFrame = false;
}

bool D3D11NvEncoder_Impl::InitializePendingFrames()
{
	m_pendingFrames = new (std::nothrow) NvEncPendingFrame[m_encodeBufferCount]{};
	return m_pendingFrames != nullptr;
}

void D3D11NvEncoder_Impl::DestroyPendingFrames()
{
	delete[] m_pendingFrames;
	m_pendingFrames = nullptr;
	::InterlockedExchange(&m_pendingFrameCount, 0);
	m_inputSequence = 0;
	m_outputSequence = 0;
}

bool D3D11NvEncoder_Impl::InitializeEncodeCompletionThread()
{
	// 동기 이벤트는 InitializeSyncEvents 가 이미 만들어 두었다.
	// 여기서 만들면 스레드와 수명이 묶여 SubmitFrame 이 닫힌 핸들을 볼 수 있다.
	if (!m_allSlotsFreeEvent || !m_frameSubmittedEvent)
		return false;

	EncodeCompletionThread* completionThread = new (std::nothrow) EncodeCompletionThread();
	if (!completionThread || !completionThread->Initialize(this))
	{
		delete completionThread;
		return false;
	}

	m_encodeCompletionThread = completionThread;
	return true;
}

void D3D11NvEncoder_Impl::StopEncodeCompletionThread()
{
	::InterlockedExchange(&m_acceptFrames, FALSE);

	if (m_encodeCompletionThread)
	{
		if (!Flush() && !IsFaulted())
		{
			printf_s("[NVENC ERROR] Flush failed while stopping encode completion thread.\n");
		}

		m_encodeCompletionThread->Shutdown();
		delete m_encodeCompletionThread;
		m_encodeCompletionThread = nullptr;
	}

	// 동기 이벤트는 여기서 닫지 않는다. Destroy 끝에서 DestroySyncEvents 가 닫는다.
}

void D3D11NvEncoder_Impl::SignalAllSlotsFree()
{
	if (m_allSlotsFreeEvent)
		::SetEvent(m_allSlotsFreeEvent);
}

NvEncOutputResult D3D11NvEncoder_Impl::ProcessOneOutput(bool block, bool invokeCallback, NvEncPacket* outPacket)
{
	if (!m_encoderHandle || !m_pendingFrames || GetPendingFrameCount() == 0)
		return NvEncOutputResult::NotReady;

	const uint32_t outputSlot = GetOutputSlotIndex();
	NvEncPendingFrame& pendingFrame = m_pendingFrames[outputSlot];

	// SubmitFrame 은 submitted = TRUE 를 기록한 뒤에 pending 을 올린다.
	// 따라서 pending > 0 인데 여기서 FALSE 가 보이면 링 장부가 깨진 것이고
	// 어느 슬롯을 회수해야 하는지 알 수 없으므로 세션을 포기한다.
	if (::InterlockedCompareExchange(&pendingFrame.submitted, TRUE, TRUE) != TRUE)
	{
		printf_s("[NVENC ERROR] Pending frame ring is inconsistent. slot=%u pending=%u\n",
			outputSlot, GetPendingFrameCount());
		EnterFaultedState(NvEncErrorCode::RingCorrupted);
		return NvEncOutputResult::Fatal;
	}

	const NvEncPacketStatus completionStatus = WaitForEncodeCompletion(outputSlot, block);

	// 논블로킹 폴링에서 아직 안 끝난 경우. 슬롯을 그대로 유지한다.
	if (completionStatus == NvEncPacketStatus::NotReady)
		return NvEncOutputResult::NotReady;

	// completion event 를 못 받았으면 NVENC 가 아직 이 슬롯의 입력 리소스를
	// 잡고 있을 수 있다. Unmap 도 슬롯 재사용도 안전하지 않으므로 복구하지 않는다.
	if (completionStatus != NvEncPacketStatus::PacketReady)
	{
		EnterFaultedState(NvEncErrorCode::OutputTimeout);
		return NvEncOutputResult::Fatal;
	}

	// 여기부터는 하드웨어 인코딩이 끝난 상태다.
	// 아래에서 실패하더라도 슬롯은 반드시 회수해서 파이프라인을 계속 돌린다.
	NvEncPacket packet = {};
	const bool packetRetrieved =
		!ConsumeDebugOutputFailure() && ReadEncodedBitstream(outputSlot, packet);

	// Unmap 실패는 매핑이 슬롯에 누적된다는 뜻이라 재사용이 불가능하다.
	if (!UnmapInputResource(outputSlot))
	{
		printf_s("[NVENC ERROR] Failed to unmap input resource. slot=%u\n", outputSlot);
		EnterFaultedState(NvEncErrorCode::OutputUnmapFailed);
		return NvEncOutputResult::Fatal;
	}

	// frameId 는 슬롯을 반납하기 전에 읽어야 한다. ClearPendingFrame 이 0 으로 지운다.
	if (packetRetrieved)
	{
		packet.frameId = pendingFrame.frameId;
		if (outPacket)
			*outPacket = packet;
	}

	// 콜백보다 슬롯 반납을 먼저 한다.
	// 콜백이 오래 걸려도 인코드 스레드가 이 슬롯에 다음 프레임을 넣을 수 있어,
	// 콜백 시간이 인코더 슬롯 점유 시간에 더해지지 않는다.
	//
	// packet.data 는 콜백 동안 계속 유효하다.
	// m_packetBuffers[slot] 에 쓰는 것은 완료 스레드 자신뿐이고,
	// 같은 슬롯의 출력을 다시 회수하려면 이 콜백이 끝나야 하기 때문이다.
	ClearPendingFrame(outputSlot);

	if (!packetRetrieved)
	{
		// 프레임 1 장만 버리고 계속 간다. 참조 프레임 체인이 끊겼을 수 있으므로
		// 앱이 키프레임을 다시 요청할 수 있도록 통지한다.
		::InterlockedIncrement64(&m_lostFrameCount);
		printf_s("[NVENC WARNING] Encoded packet dropped. slot=%u\n", outputSlot);
		InvokeErrorCallback(NvEncErrorCode::OutputReadFailed);
		return NvEncOutputResult::FrameLost;
	}

	::InterlockedIncrement64(&m_completedFrameCount);

	if (invokeCallback)
		InvokeEncodedPacketCallback(packet);

	return NvEncOutputResult::Completed;
}

void D3D11NvEncoder_Impl::ClearPendingFrame(uint32_t slot)
{
	// 슬롯을 비우고 pending 을 내린다. 성공/실패 어느 경로에서도 반드시 불려야 한다.
	NvEncPendingFrame& pendingFrame = m_pendingFrames[slot];
	pendingFrame.frameId = 0;
	::InterlockedExchange(&pendingFrame.submitted, FALSE);
	++m_outputSequence;

	if (::InterlockedDecrement(&m_pendingFrameCount) == 0)
		SignalAllSlotsFree();
}

void D3D11NvEncoder_Impl::AbortPendingFrames()
{
	// 파이프라인을 포기하는 경로.
	// NVENC 가 아직 슬롯을 잡고 있을 수 있으므로 Unmap 은 시도하지 않는다.
	// 매핑은 Destroy 의 DestroyMappedInputBuffers 가 정리한다.
	// 여기서는 WaitForPendingFrames 대기자가 영원히 멈추지 않도록 장부만 비운다.
	if (m_pendingFrames)
	{
		for (uint32_t i = 0; i < m_encodeBufferCount; ++i)
		{
			if (::InterlockedExchange(&m_pendingFrames[i].submitted, FALSE) == TRUE)
				::InterlockedIncrement64(&m_lostFrameCount);

			m_pendingFrames[i].frameId = 0;
		}
	}

	::InterlockedExchange(&m_pendingFrameCount, 0);
	SignalAllSlotsFree();
}

void D3D11NvEncoder_Impl::EnterFaultedState(NvEncErrorCode errorCode)
{
	// 더 이상 프레임을 받지 않는다. CanSubmitFrame 이 결정적으로 false 가 되어
	// 호출자가 조용히 드롭되는 대신 실패를 관측할 수 있다.
	::InterlockedExchange(&m_acceptFrames, FALSE);
	const bool alreadyFaulted = (::InterlockedExchange(&m_faulted, TRUE) == TRUE);

	AbortPendingFrames();

	if (!alreadyFaulted)
	{
		printf_s("[NVENC ERROR] Encoder entered faulted state. errorCode=%u\n",
			static_cast<uint32_t>(errorCode));
		InvokeErrorCallback(errorCode);
	}
}

void D3D11NvEncoder_Impl::InvokeEncodedPacketCallback(const NvEncPacket& packet)
{
	::AcquireSRWLockShared(&m_callbackLock);
	if (m_encodedPacketCallback)
		m_encodedPacketCallback(packet, m_encodedPacketCallbackUserData);
	::ReleaseSRWLockShared(&m_callbackLock);
}

void D3D11NvEncoder_Impl::InvokeErrorCallback(NvEncErrorCode errorCode)
{
	::AcquireSRWLockShared(&m_callbackLock);
	if (m_errorCallback)
		m_errorCallback(errorCode, m_errorCallbackUserData);
	::ReleaseSRWLockShared(&m_callbackLock);
}

bool D3D11NvEncoder_Impl::RegisterResource(void* buffer, NV_ENC_INPUT_RESOURCE_TYPE eResourceType, uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT eBufferFormat, NV_ENC_BUFFER_USAGE eBufferUsage, NV_ENC_REGISTERED_PTR& registeredResource)
{
	// NVENC 내부 리소스로 사용하기 위한 리소스 등록을 도와주는 래핑 함수

	NV_ENC_REGISTER_RESOURCE registerResource = { NV_ENC_REGISTER_RESOURCE_VER };
	registerResource.resourceType = eResourceType;
	registerResource.resourceToRegister = buffer;
	registerResource.width = width;
	registerResource.pitch = pitch;
	registerResource.height = height;
	registerResource.bufferFormat = eBufferFormat;
	registerResource.bufferUsage = eBufferUsage;
	registerResource.pInputFencePoint = nullptr;
	if (!NVENC_API_CALL(m_nvenc.nvEncRegisterResource(m_encoderHandle, &registerResource)))
		return false;

	registeredResource = registerResource.registeredResource;
	return true;
}

bool D3D11NvEncoder_Impl::RegisterInputResources(void** inputFrames, uint32_t inputFrameCount, NV_ENC_INPUT_RESOURCE_TYPE eResourceType, uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT eBufferFormat)
{
	// 외부의 D3D11 BGRA Texture 를 넘겨받아 NVENC 가 접근 가능하도록 리소스로 등록한다.
	// inputFrames 는 프로그램 종료 시점까지 해제되지 않으며, 고정된 크기와 수량의 버퍼로 생성되어 있어야 한다.

	if (!inputFrames || !m_registeredResources)
	{
		return false;
	}

	// 외부에서 받은 프레임(D3D11Texture2D NV12) 를 NVENC 의 Input 으로 Register 수행
	for (uint32_t i = 0; i < inputFrameCount; ++i)
	{
		NV_ENC_REGISTERED_PTR registeredPtr = nullptr;
		if (!RegisterResource(inputFrames[i], eResourceType, width, height, pitch, eBufferFormat, NV_ENC_INPUT_IMAGE, registeredPtr))
		{
			for (uint32_t cleanupSlot = 0; cleanupSlot < i; ++cleanupSlot)
			{
				if (m_registeredResources[cleanupSlot])
				{
					NVENC_API_CALL(m_nvenc.nvEncUnregisterResource(m_encoderHandle, m_registeredResources[cleanupSlot]));
					m_registeredResources[cleanupSlot] = nullptr;
				}
			}
			return false;
		}

		m_registeredResources[i] = registeredPtr;
	}

	return true;
}

bool D3D11NvEncoder_Impl::SetBGRAInputTexture(ID3D11Texture2D** textures, uint32_t bufferCount)
{
	// 외부의 BGRA D3D11 Texture 를 받아서 컨버터 Input 으로 설정
	if (!m_converter)
		return false;

	return m_converter->SetInputTextures(textures, bufferCount);
}

bool D3D11NvEncoder_Impl::SetNV12OutputTexture(ID3D11Texture2D** textures, uint32_t bufferCount)
{
	// 외부의 NV12 D3D11 Texture 를 받아서 컨버터 Output 으로 설정
	if (!m_converter)
		return false;

	return m_converter->SetOutputTextures(textures, bufferCount);
}

bool D3D11NvEncoder_Impl::EncodePicture(uint32_t slot)
{
	// 사전에 Registered 된 Input Resource 의 Texture 를 Encode 한다.
	// 여기서의 Input 은 NV12 타입일 것이고, Output 은 H264 로 Encode 된 Bitstream Buffer 이다.
	if (!m_encoderHandle || !m_mappedInputBuffers || !m_bitstreamBuffers || slot >= m_encodeBufferCount)
	{
		return false;
	}

	// Input, Output 버퍼를 가져와서 Encode Request
	NV_ENC_INPUT_PTR inputBuffer = m_mappedInputBuffers[slot];
	NV_ENC_OUTPUT_PTR outputBuffer = m_bitstreamBuffers[slot];
	if (!inputBuffer || !outputBuffer)
		return false;

	NV_ENC_PIC_PARAMS picParams = {};
	picParams.version = NV_ENC_PIC_PARAMS_VER;
	picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	picParams.inputTimeStamp = m_timeStamp++;
	picParams.inputBuffer = inputBuffer;
	picParams.bufferFmt = GetPixelFormat();
	picParams.inputWidth = GetEncodeWidth();
	picParams.inputHeight = GetEncodeHeight();
	//picParams.inputPitch = GetEncodeWidth();
	picParams.frameIdx = m_inputSequence;
	picParams.outputBitstream = outputBuffer;
	picParams.completionEvent = GetCompletionEvent(slot);
	const bool forceKeyFrame = (::InterlockedExchange(&m_forceKeyFrame, FALSE) == TRUE);
	if (forceKeyFrame)
	{
		picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
	}

	NVENCSTATUS nvStatus = NV_ENC_ERR_GENERIC;
	{
		// NVENC may access the registered D3D11 resource and its immediate
		// context internally while submitting the encode request.
		D3D11ImmediateContextGuard contextGuard(m_contextGate);
		nvStatus = m_nvenc.nvEncEncodePicture(m_encoderHandle, &picParams);
	}
	if (nvStatus == NV_ENC_ERR_NEED_MORE_INPUT)
	{
		// NVENC 가 입력은 받았지만 이 프레임에 대한 출력은 지금 내주지 않는다는 뜻이다.
		// B 프레임이나 lookahead 를 켰을 때 발생한다.
		//
		// 이 구현은 슬롯 하나 = 출력 하나를 가정한다(m_pendingFrames 링).
		// 여기서 true 를 돌려주면 SubmitFrame 이 pending 을 올리고,
		// 그 슬롯의 completion event 는 영원히 signal 되지 않아
		// 완료 스레드가 20 초 타임아웃 후 세션을 포기한다.
		//
		// 현재 설정(frameIntervalP = 1, lookahead 없음)에서는 발생하지 않는다.
		// 발생했다면 지원하지 않는 설정이므로 조용히 매달리는 대신 즉시 실패한다.
		// B 프레임을 지원하려면 "출력 없는 제출"을 표현하는 모델이 먼저 필요하다.
		printf_s("[NVENC ERROR] Encoder returned NEED_MORE_INPUT."
			" B-frames / lookahead are not supported by this pipeline.\n");

		if (forceKeyFrame)
			::InterlockedExchange(&m_forceKeyFrame, TRUE);

		return false;
	}

	const bool encodeSucceeded = NVENC_API_CALL(nvStatus);
	if (!encodeSucceeded && forceKeyFrame)
	{
		::InterlockedExchange(&m_forceKeyFrame, TRUE);
	}

	return encodeSucceeded;
}

NvEncPacketStatus D3D11NvEncoder_Impl::WaitForEncodeCompletion(uint32_t slot, bool block)
{
	// Async Encode 를 사용하는 경우
	// Encode 완료 이벤트를 NVENC 내부에서 Set 해준다.
	// 이 Event 를 대기하여 동기를 맞춘다.
	if (m_initParameters.enableEncodeAsync == 0U)
	{
		return NvEncPacketStatus::PacketReady;
	}

	HANDLE completionEvent = GetCompletionEvent(slot);
	if (!completionEvent)
		return NvEncPacketStatus::Error;

	const DWORD timeoutMilliseconds = block ? 20'000U : 0U;
	const DWORD dwResult = ::WaitForSingleObject(completionEvent, timeoutMilliseconds);

	if (dwResult == WAIT_OBJECT_0)
		return NvEncPacketStatus::PacketReady;

	if (dwResult == WAIT_TIMEOUT && !block)
		return NvEncPacketStatus::NotReady;

	if (dwResult == WAIT_FAILED)
	{
		printf_s("[NVENC ERROR] Failed to encode frame.\n");
	}
	else if (dwResult == WAIT_TIMEOUT)
	{
		printf_s("[NVENC ERROR] Timeout encode frame.\n");
	}

	return NvEncPacketStatus::Error;
}

bool D3D11NvEncoder_Impl::ReadEncodedBitstream(uint32_t slot, NvEncPacket& packet)
{
	// Encode 완료를 기다리고 H264 로 Encode 된 Bitstream Buffer 를 읽어온다.
	if (!m_bitstreamBuffers || !m_packetBuffers || slot >= m_encodeBufferCount)
		return false;

	// Encode Result 를 가져오기 위해 NVENC 내부 Bitstream Buffer Lock
	//
	// Lock/Unlock 이 D3D11 컨텍스트를 타는지는 드라이버 내부라 확인할 수 없다.
	// 비트스트림 버퍼는 NVENC 가 자체 할당한 것이라 등록된 D3D11 리소스를
	// 만지지 않을 가능성이 높지만, 게이트 획득 비용이 수십 ns 수준이므로
	// multithread protection 을 끈 환경에서는 방어적으로 감싸는 편이 낫다.
	// Lock -> memcpy -> Unlock 을 한 번의 획득으로 묶어 시퀀스 원자성도 얻는다.
	D3D11ImmediateContextGuard contextGuard(m_contextGate);

	NV_ENC_LOCK_BITSTREAM lockBitstreamData = {};
	lockBitstreamData.version = NV_ENC_LOCK_BITSTREAM_VER;
	lockBitstreamData.outputBitstream = m_bitstreamBuffers[slot];
	lockBitstreamData.doNotWait = false;
	if (!NVENC_API_CALL(m_nvenc.nvEncLockBitstream(m_encoderHandle, &lockBitstreamData)))
		return false;

	// Bitstream Result 를 저장하기 위한 frame 획득
	NvEncPacketBuffer& frame = m_packetBuffers[slot];

	// frame 이 처음 사용되거나 공간이 부족한 경우 기존 메모리 해제 후 재할당
	// 확보된 메모리에 결과를 Copy 한다.
	if (frame.streamDataCapacity < lockBitstreamData.bitstreamSizeInBytes)
	{
		delete[] frame.streamData;
		frame.streamData = new (std::nothrow) uint8_t[lockBitstreamData.bitstreamSizeInBytes];
		if (!frame.streamData)
		{
			frame.streamDataCapacity = 0;
			frame.streamDataSize = 0;
			NVENC_API_CALL(m_nvenc.nvEncUnlockBitstream(m_encoderHandle, lockBitstreamData.outputBitstream));
			return false;
		}
		frame.streamDataCapacity = lockBitstreamData.bitstreamSizeInBytes;
	}

	// NVENC 내부 Bitstream Buffer 로부터 Encode 결과를 Copy 한다.
	memcpy(frame.streamData, lockBitstreamData.bitstreamBufferPtr, lockBitstreamData.bitstreamSizeInBytes);

	// 데이터 외 기타 정보 복사
	frame.streamDataSize = lockBitstreamData.bitstreamSizeInBytes;
	frame.pictureType = lockBitstreamData.pictureType;
	frame.timeStamp = lockBitstreamData.outputTimeStamp;
	frame.isKeyFrame = (lockBitstreamData.pictureType == NV_ENC_PIC_TYPE_IDR);

	packet.data = frame.streamData;
	packet.size = frame.streamDataSize;
	packet.timestamp = frame.timeStamp;
	packet.frameType = static_cast<uint16_t>(frame.pictureType);
	packet.isKeyFrame = frame.isKeyFrame;

	// Bitstream Buffer Unlock
	return NVENC_API_CALL(m_nvenc.nvEncUnlockBitstream(m_encoderHandle, lockBitstreamData.outputBitstream));
}

bool D3D11NvEncoder_Impl::Flush()
{
	// EOS 보내서 Encoder 내부 버퍼를 비워준다.

	if (!m_encoderHandle)
		return true;

	// 이미 fault 상태면 EOS 를 보내도 회수할 출력이 없고 대기만 길어진다.
	if (IsFaulted())
		return false;

	NV_ENC_PIC_PARAMS picParams = {};
	picParams.version = NV_ENC_PIC_PARAMS_VER;
	picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
	picParams.inputPitch = 0;
	picParams.completionEvent = m_eosCompletionEvent;

	bool sendSucceeded = false;
	{
		D3D11ImmediateContextGuard contextGuard(m_contextGate);
		sendSucceeded = NVENC_API_CALL(
			m_nvenc.nvEncEncodePicture(m_encoderHandle, &picParams));
	}
	if (!sendSucceeded)
		return false;

	if (m_asyncPipelineEnabled)
	{
		if (m_frameSubmittedEvent)
			::SetEvent(m_frameSubmittedEvent);
		if (!WaitForPendingFrames(20'000U))
			return false;
	}
	else
	{
		while (GetPendingFrameCount() > 0)
		{
			// FrameLost 도 슬롯을 회수하므로 루프는 계속 진행된다.
			const NvEncOutputResult result = ProcessOneOutput(true, false);
			if (result == NvEncOutputResult::Fatal || result == NvEncOutputResult::NotReady)
				return false;
		}
	}

	if (m_initParameters.enableEncodeAsync == 0U)
		return true;

	if (!m_eosCompletionEvent)
		return false;

	const DWORD waitResult = ::WaitForSingleObject(m_eosCompletionEvent, 20'000U);
	if (waitResult != WAIT_OBJECT_0)
	{
		printf_s("[NVENC ERROR] Failed to flush encoder. waitResult=%lu\n", waitResult);
		return false;
	}

	return true;
}

uint32_t D3D11NvEncoder_Impl::GetInputSlotIndex() const
{
	return WrapRingIndex(m_inputSequence, m_encodeBufferCount);
}

uint32_t D3D11NvEncoder_Impl::GetOutputSlotIndex() const
{
	return WrapRingIndex(m_outputSequence, m_encodeBufferCount);
}

bool D3D11NvEncoder_Impl::MapInputResource(uint32_t slot)
{
	// EncodeFrame 가 호출 되기 전에 Input Resource Map 수행
	if (!m_registeredResources || !m_mappedInputBuffers || slot >= m_encodeBufferCount)
		return false;

	NV_ENC_MAP_INPUT_RESOURCE mapInputResource = { };

	mapInputResource.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
	mapInputResource.registeredResource = m_registeredResources[slot];
	bool mapSucceeded = false;
	{
		D3D11ImmediateContextGuard contextGuard(m_contextGate);
		mapSucceeded = NVENC_API_CALL(
			m_nvenc.nvEncMapInputResource(m_encoderHandle, &mapInputResource));
	}
	if (!mapSucceeded)
		return false;

	m_mappedInputBuffers[slot] = mapInputResource.mappedResource;
	return true;
}

bool D3D11NvEncoder_Impl::UnmapInputResource(uint32_t slot)
{
	// EncodeFrame 완료된 후 Input Resource Unmap 수행
	if (!m_mappedInputBuffers || slot >= m_encodeBufferCount)
		return false;

	if (m_mappedInputBuffers[slot])
	{
		bool unmapSucceeded = false;
		{
			D3D11ImmediateContextGuard contextGuard(m_contextGate);
			unmapSucceeded = NVENC_API_CALL(
				m_nvenc.nvEncUnmapInputResource(m_encoderHandle, m_mappedInputBuffers[slot]));
		}
		if (!unmapSucceeded)
			return false;

		m_mappedInputBuffers[slot] = nullptr;
	}

	return true;
}

//void SimpleNvEncoderD3D11::Reconfigure(uint32_t bitrate)
//{
//    m_config.rcParams.averageBitRate = bitrate;
//
//    NV_ENC_RECONFIGURE_PARAMS params = {
//        NV_ENC_RECONFIGURE_PARAMS_VER
//    };
//    params.reInitEncodeParams = m_initParams;
//    params.reInitEncodeParams.encodeConfig = &m_config;
//
//    NVENC_API_CALL(
//        m_nvenc.nvEncReconfigureEncoder(m_encoderHandle, &params)
//    );
//}

int32_t D3D11NvEncoder_Impl::GetCapabilityValue(GUID guidCodec, NV_ENC_CAPS capsToQuery)
{
	if (!m_encoderHandle)
	{
		return 0;
	}

	NV_ENC_CAPS_PARAM capsParam = { NV_ENC_CAPS_PARAM_VER };
	capsParam.capsToQuery = capsToQuery;
	int32_t value = 0;
	m_nvenc.nvEncGetEncodeCaps(m_encoderHandle, guidCodec, &capsParam, &value);

	return value;
}

inline uint32_t D3D11NvEncoder_Impl::GetEncodeWidth() const
{
	return m_initParameters.encodeWidth;
}

inline uint32_t D3D11NvEncoder_Impl::GetEncodeHeight() const
{
	return  m_initParameters.encodeHeight;
}

uint32_t D3D11NvEncoder_Impl::GetMaxEncodeWidth() const
{
	return m_initParameters.maxEncodeWidth;
}

uint32_t D3D11NvEncoder_Impl::GetMaxEncodeHeight() const
{
	return m_initParameters.maxEncodeHeight;
}

NV_ENC_BUFFER_FORMAT D3D11NvEncoder_Impl::GetPixelFormat() const
{
	return m_initParameters.bufferFormat;
}

DXGI_FORMAT D3D11NvEncoder_Impl::GetD3D11Format(NV_ENC_BUFFER_FORMAT eBufferFormat) const
{
	switch (eBufferFormat)
	{
	case NV_ENC_BUFFER_FORMAT_NV12:
		return DXGI_FORMAT_NV12;
	case NV_ENC_BUFFER_FORMAT_ARGB:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

HANDLE D3D11NvEncoder_Impl::GetCompletionEvent(uint32_t slot)
{
	return m_slotCompletionEvents && slot < m_encodeBufferCount ? m_slotCompletionEvents[slot] : nullptr;
}

