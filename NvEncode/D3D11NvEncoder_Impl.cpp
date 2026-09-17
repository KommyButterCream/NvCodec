#include "pch.h"
#include "D3D11NvEncoder_Impl.h"
#include "D3D11NvEncoder.h"   // for NVENC_SHARED_INPUT_MUTEX_*

// ID3D11Device1::OpenSharedResource1 과 IDXGIKeyedMutex 를 쓴다.
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include "../../D3D11EngineInterface/ID3D11ImmediateContextGate.h"
#include "D3D11VideoProcessorNV12.h"
#include "EncodeCompletionThread.h"
#include "EncodeThread.h"
#include "EncodeFrameQueue.h"

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

	// 저지연 목적에서는 프리셋보다 튜닝이 지연을 결정한다.
	// UltraLow 와 Low 가 같은 프리셋을 쓰는 것은 의도적이다 —
	// 프리셋을 낮추면 화질이 같이 떨어지므로, 먼저 튜닝만 바꿔 효과를 재고
	// 필요하면 그때 프리셋을 조정한다.
	GUID ToPresetGuid(NvEncLatencyMode mode)
	{
		switch (mode)
		{
		case NvEncLatencyMode::UltraLow: return NV_ENC_PRESET_P3_GUID;
		case NvEncLatencyMode::Low:      return NV_ENC_PRESET_P3_GUID;
		case NvEncLatencyMode::Quality:  return NV_ENC_PRESET_P5_GUID;
		default:                         return NV_ENC_PRESET_P3_GUID;
		}
	}

	NV_ENC_TUNING_INFO ToTuningInfo(NvEncLatencyMode mode)
	{
		switch (mode)
		{
		case NvEncLatencyMode::UltraLow: return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
		case NvEncLatencyMode::Low:      return NV_ENC_TUNING_INFO_LOW_LATENCY;
		case NvEncLatencyMode::Quality:  return NV_ENC_TUNING_INFO_HIGH_QUALITY;
		default:                         return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
		}
	}

	GUID ToProfileGuid(NvEncH264Profile profile)
	{
		switch (profile)
		{
		case NvEncH264Profile::Baseline: return NV_ENC_H264_PROFILE_BASELINE_GUID;
		case NvEncH264Profile::Main:     return NV_ENC_H264_PROFILE_MAIN_GUID;
		case NvEncH264Profile::High:     return NV_ENC_H264_PROFILE_HIGH_GUID;
		default:                         return NV_ENC_H264_PROFILE_HIGH_GUID;
		}
	}

	NV_ENC_PARAMS_RC_MODE ToRateControlMode(NvEncRateControl rateControl)
	{
		switch (rateControl)
		{
		case NvEncRateControl::ConstantBitrate: return NV_ENC_PARAMS_RC_CBR;
		case NvEncRateControl::VariableBitrate: return NV_ENC_PARAMS_RC_VBR;
		case NvEncRateControl::ConstantQP:      return NV_ENC_PARAMS_RC_CONSTQP;
		default:                                return NV_ENC_PARAMS_RC_CBR;
		}
	}

	// 저지연 기본값: VBV 를 1 프레임 분량으로 잡는다.
	// 이보다 크면 복잡한 장면에서 프레임 하나가 부풀어 전송이 늦어지고,
	// 작으면 화질이 과하게 떨어진다.
	uint32_t ComputeSingleFrameVbvBits(uint32_t bitrateBps, uint32_t fpsNum, uint32_t fpsDen)
	{
		if (bitrateBps == 0 || fpsNum == 0 || fpsDen == 0)
			return 0;

		const uint64_t bits =
			(static_cast<uint64_t>(bitrateBps) * static_cast<uint64_t>(fpsDen))
			/ static_cast<uint64_t>(fpsNum);

		return static_cast<uint32_t>(bits);
	}
}
// =============================================================================
// 생성 / 소멸
// =============================================================================

D3D11NvEncoder_Impl::~D3D11NvEncoder_Impl()
{
	Destroy();
}

// =============================================================================
// 초기화 / 종료
// =============================================================================

bool D3D11NvEncoder_Impl::Initialize(
	ID3D11Device* device,
	const NvEncConfig& config,
	ID3D11ImmediateContextGate* contextGate)
{
	// encodeSlotCount 가 1 이면 제출 -> 대기 -> 완료가 완전히 직렬화되어
	// async 파이프라인의 의미가 사라진다. 최소 2 를 요구한다.
	if (!device || config.width == 0 || config.height == 0 ||
		config.encodeSlotCount < kMinEncodeBufferCount || !IsPowerOfTwo(config.encodeSlotCount))
	{
		printf_s("[NVENC ERROR] Invalid encoder parameters. width=%u height=%u encodeSlotCount=%u"
			" (encodeSlotCount must be a power of two and at least %u)\n",
			config.width, config.height, config.encodeSlotCount, kMinEncodeBufferCount);
		return false;
	}

	if (config.averageBitrateBps == 0
		|| config.frameRateNumerator == 0
		|| config.frameRateDenominator == 0)
	{
		printf_s("[NVENC ERROR] Invalid encoder parameters."
			" bitrate=%u frameRate=%u/%u (all must be non-zero)\n",
			config.averageBitrateBps, config.frameRateNumerator, config.frameRateDenominator);
		return false;
	}

	// maxWidth / maxHeight 는 런타임 해상도 변경의 상한이다.
	// 지정했다면 현재 해상도보다 작을 수 없다.
	if ((config.maxWidth > 0 && config.maxWidth < config.width)
		|| (config.maxHeight > 0 && config.maxHeight < config.height))
	{
		printf_s("[NVENC ERROR] maxWidth/maxHeight (%ux%u) must not be smaller than width/height (%ux%u).\n",
			config.maxWidth, config.maxHeight, config.width, config.height);
		return false;
	}

	if (config.enableIntraRefresh && config.intraRefreshPeriodFrames < 2)
	{
		printf_s("[NVENC ERROR] intraRefreshPeriodFrames must be at least 2.\n");
		return false;
	}

	Destroy();

	// 건네 받은 D3D11 Device, Context 포인터의 참조 횟수 증가
	// Destroy 시점에 Release 호출 필요
	m_D3D11Device = device;
	m_D3D11Device->AddRef();
	m_D3D11Device->GetImmediateContext(&m_D3D11Context);
	m_contextGate = contextGate;

	m_userConfig = config;
	m_width = config.width;
	m_height = config.height;
	m_encodeSlotCount = config.encodeSlotCount;
	m_asyncPipelineEnabled = config.enableAsyncPipeline;

	m_timestamp = 0;
	m_inputSequence = 0;
	m_outputSequence = 0;
	::InterlockedExchange(&m_pendingFrameCount, 0);
	::InterlockedExchange(&m_forceKeyFrame, FALSE);
	::InterlockedExchange(&m_acceptFrames, FALSE);

	// 재초기화면 앞서 등록된 공유 풀은 무효다. RegisterSharedInputPool 을
	// 다시 불러야 한다.
	UnregisterSharedInputPool();
	::InterlockedExchange(&m_faulted, FALSE);
	::InterlockedExchange(&m_debugFailOutputCount, 0);
	::InterlockedExchange64(&m_submittedFrameCount, 0);
	::InterlockedExchange64(&m_completedFrameCount, 0);
	::InterlockedExchange64(&m_lostFrameCount, 0);


	if (!CreateSyncEvents())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreateSyncEvents.\n");
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
		if (!CreateEncoderResources())
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
	if (m_asyncPipelineEnabled && !CreateEncodeCompletionThread())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreateEncodeCompletionThread.\n");
		Destroy();
		return false;
	}

	// 유입 큐. 소비자가 인코드 스레드 하나뿐이라 여기서 만들어 소유한다.
	// 동기 파이프라인에는 큐가 없다 — 호출자가 직접 EncodeSync 를 돌린다.
	if (m_asyncPipelineEnabled && !CreateInputQueue(config.inputQueueDepth))
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreateInputQueue.\n");
		Destroy();
		return false;
	}

	::InterlockedExchange(&m_acceptFrames, TRUE);
	return true;
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

// 게이트를 획득한 상태에서 호출된다. 내부에서 게이트를 다시 잡아서는 안 된다.
bool D3D11NvEncoder_Impl::CreateEncoderResources()
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

	if (!CreateBGRAToNV12Converter())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreateBGRAToNV12Converter.\n");
		goto fail_converter;
	}

	if (!CreateAsyncEvent())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreateAsyncEvent.\n");
		goto fail_async_event;
	}

	if (!CreateBitstreamBuffers())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreateBitstreamBuffers.\n");
		goto fail_bitstream;
	}

	if (!CreateRegisteredResources())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreateRegisteredResources.\n");
		goto fail_registered_resources;
	}

	if (!CreateD3D11InputBuffers())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreateD3D11InputBuffers.\n");
		goto fail_input_buffers;
	}

	if (!CreateMappedInputBuffers())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreateMappedInputBuffers.\n");
		goto fail_mapped_inputs;
	}

	if (!CreatePacketBuffers())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreatePacketBuffers.\n");
		goto fail_output_frames;
	}

	if (!CreatePendingFrames())
	{
		printf_s("[NVENC ERROR] Initialize stage failed: CreatePendingFrames.\n");
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
	DestroyBGRAToNV12Converter();
fail_converter:
	DestroyEncoder();
	return false;
fail_device:
	DestroyEncoder();
	return false;
}

void D3D11NvEncoder_Impl::ApplyStaticConfig(const NvEncConfig& config)
{
	// 프리셋 / 튜닝 / 프로파일 / GOP 구조.
	// 전부 nvEncReconfigureEncoder 로는 바꿀 수 없는 항목이다.

	m_initParameters.presetGUID = ToPresetGuid(config.latencyMode);
	m_initParameters.tuningInfo = ToTuningInfo(config.latencyMode);
	m_config.profileGUID = ToProfileGuid(config.profile);

	NV_ENC_CONFIG_H264& h264 = m_config.encodeCodecConfig.h264Config;
	h264.chromaFormatIDC = 1;
	h264.repeatSPSPPS = config.repeatSequenceHeader ? 1U : 0U;

	// 스트림에 색공간을 명시한다. 없으면 수신측이 추측하고 색이 틀어진다.
	// 컨버터가 RGB full range 를 BT.709 로 변환하므로 그에 맞춘다.
	h264.h264VUIParameters.videoSignalTypePresentFlag = 1;
	h264.h264VUIParameters.videoFullRangeFlag = 1;
	h264.h264VUIParameters.colourDescriptionPresentFlag = 1;
	h264.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
	h264.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
	h264.h264VUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;

	// B 프레임은 쓰지 않는다. 슬롯 하나에 출력 하나를 가정한 링 구조이고,
	// B 프레임은 NEED_MORE_INPUT 을 유발해 그 가정을 깬다.
	m_config.frameIntervalP = 1;

	if (config.enableIntraRefresh)
	{
		// 주기적 IDR 대신 매 프레임의 일부만 intra 로 인코딩해서
		// 프레임 크기를 균일하게 만든다. IDR 주기마다 지연이 튀는 것을 없앤다.
		//
		// SDK 제약: gopLength 가 NVENC_INFINITE_GOPLENGTH 가 아니면
		// intra refresh 는 무시된다. idrPeriod 도 같이 무한으로 둬야
		// 드라이버가 IDR 을 끼워넣지 않는다.
		m_config.gopLength = NVENC_INFINITE_GOPLENGTH;
		h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
		h264.enableIntraRefresh = 1;
		h264.intraRefreshPeriod = config.intraRefreshPeriodFrames;

		// intraRefreshCnt 는 intraRefreshPeriod 보다 작아야 한다(SDK).
		// 한 주기 전체에 걸쳐 퍼뜨려야 프레임당 intra 영역이 가장 작아진다.
		h264.intraRefreshCnt = (config.intraRefreshPeriodFrames > 1)
			? config.intraRefreshPeriodFrames - 1U
			: 1U;
	}
	else
	{
		// intra refresh 를 쓰지 않으면 gopLength 와 idrPeriod 를 일치시킨다.
		// 서로 다르면 IDR 이 아닌 I 프레임이 중간에 끼는데, 그 프레임은
		// IDR 만큼 크면서 신규 수신자의 진입점이 되지 못한다.
		m_config.gopLength = config.gopLengthFrames;
		h264.idrPeriod = config.gopLengthFrames;
		h264.enableIntraRefresh = 0;
		h264.intraRefreshPeriod = 0;
		h264.intraRefreshCnt = 0;
	}

	m_initParameters.encodeGUID = NV_ENC_CODEC_H264_GUID;
	m_initParameters.encodeWidth = config.width;
	m_initParameters.encodeHeight = config.height;
	m_initParameters.darWidth = config.width;
	m_initParameters.darHeight = config.height;
	m_initParameters.maxEncodeWidth = (config.maxWidth > 0) ? config.maxWidth : config.width;
	m_initParameters.maxEncodeHeight = (config.maxHeight > 0) ? config.maxHeight : config.height;
	m_initParameters.enablePTD = 1;
	m_initParameters.reportSliceOffsets = 0;
	m_initParameters.enableSubFrameWrite = 0;
	m_initParameters.enableMEOnlyMode = false;
	m_initParameters.enableOutputInVidmem = false;
	m_initParameters.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
}

void D3D11NvEncoder_Impl::ApplyRateControlConfig(const NvEncConfig& config)
{
	// nvEncReconfigureEncoder 로 갱신할 수 있는 항목만 여기서 다룬다.

	m_initParameters.frameRateNum = config.frameRateNumerator;
	m_initParameters.frameRateDen = config.frameRateDenominator;

	NV_ENC_RC_PARAMS& rc = m_config.rcParams;
	rc.rateControlMode = ToRateControlMode(config.rateControl);
	rc.averageBitRate = config.averageBitrateBps;
	rc.maxBitRate = (config.maxBitrateBps > 0) ? config.maxBitrateBps : config.averageBitrateBps;

	// VBV(HRD) 버퍼는 인코더가 한 번에 몰아 쓸 수 있는 비트 예산이다.
	// 프리셋 기본값은 프리셋 자신의 비트레이트 기준으로 계산돼 있어서,
	// averageBitRate 만 덮어쓰면 둘이 어긋난 채로 남는다.
	// 저지연에서는 1 프레임 분량이 정석이다.
	rc.vbvBufferSize = (config.vbvBufferSizeBits > 0)
		? config.vbvBufferSizeBits
		: ComputeSingleFrameVbvBits(
			config.averageBitrateBps, config.frameRateNumerator, config.frameRateDenominator);
	rc.vbvInitialDelay = rc.vbvBufferSize;

	// 단일 프레임 VBV + CBR 에서 I 프레임이 P 프레임 대비 몇 배까지
	// 비트를 쓸 수 있는지. 1 이면 키프레임도 예산을 넘기지 않아
	// 키프레임 구간의 지연 스파이크가 사라진다.
	rc.lowDelayKeyFrameScale = (config.latencyMode == NvEncLatencyMode::UltraLow) ? 1U : 2U;

	rc.enableMinQP = (config.minQP > 0) ? 1U : 0U;
	rc.enableMaxQP = (config.maxQP > 0) ? 1U : 0U;
	if (config.minQP > 0)
		rc.minQP = { config.minQP, config.minQP, config.minQP };
	if (config.maxQP > 0)
		rc.maxQP = { config.maxQP, config.maxQP, config.maxQP };

	rc.targetQuality = (config.rateControl == NvEncRateControl::VariableBitrate)
		? config.targetQuality : 0U;

	if (config.rateControl == NvEncRateControl::ConstantQP)
		rc.constQP = { config.constantQP, config.constantQP, config.constantQP };

	rc.enableAQ = config.enableAdaptiveQuantization ? 1U : 0U;
}

bool D3D11NvEncoder_Impl::InitializeEncoder()
{
	// 설정을 NVENC 파라메터로 옮기고 Encoder 를 생성한다.
	if (!m_encoderHandle)
	{
		printf_s("[NVENC ERROR] Encoder handle is not initialized.\n");
		return false;
	}

	memset(&m_initParameters, 0, sizeof(m_initParameters));
	memset(&m_config, 0, sizeof(m_config));

	// 프리셋 기본값을 먼저 깔고 그 위에 우리 설정을 덮는다.
	// 여기서 채워지는 값 중 우리가 건드리지 않는 것들(모션 추정, 엔트로피 코딩 등)은
	// 프리셋의 판단을 그대로 쓴다.
	NV_ENC_PRESET_CONFIG presetConfig = {};
	presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
	presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

	if (!NVENC_API_CALL(m_nvenc.nvEncGetEncodePresetConfigEx(
		m_encoderHandle,
		NV_ENC_CODEC_H264_GUID,
		ToPresetGuid(m_userConfig.latencyMode),
		ToTuningInfo(m_userConfig.latencyMode),
		&presetConfig)))
	{
		return false;
	}

	memcpy(&m_config, &presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
	m_config.version = NV_ENC_CONFIG_VER;

	m_initParameters.version = NV_ENC_INITIALIZE_PARAMS_VER;
	m_initParameters.encodeConfig = &m_config;

	ApplyStaticConfig(m_userConfig);
	ApplyRateControlConfig(m_userConfig);

	// async 여부는 하드웨어 지원에 달려 있다. 지원하지 않으면 0 이 되고
	// completion event 없이 blocking lock 으로 동작한다.
	m_initParameters.enableEncodeAsync = m_asyncPipelineEnabled
		? GetCapabilityValue(NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS::NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT)
		: 0U;

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

bool D3D11NvEncoder_Impl::CreateSyncEvents()
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

bool D3D11NvEncoder_Impl::CreateAsyncEvent()
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
	m_slotCompletionEvents = new (std::nothrow) HANDLE[m_encodeSlotCount]{};
	if (!m_slotCompletionEvents)
		return false;

	for (uint32_t i = 0; i < m_encodeSlotCount; i++)
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
		for (uint32_t i = 0; i < m_encodeSlotCount; i++)
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

bool D3D11NvEncoder_Impl::CreateMappedInputBuffers()
{
	// NVENC Encoding 을 위한 Input Buffer 를 미리 생성한다.
	// Encode 수행 함수를 호출할때 NV_ENC_INPUT_PTR 타입 필요.
	m_mappedInputBuffers = new (std::nothrow) NV_ENC_INPUT_PTR[m_encodeSlotCount]{};
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

	for (uint32_t i = 0; i < m_encodeSlotCount; i++)
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

bool D3D11NvEncoder_Impl::CreateBitstreamBuffers()
{
	// NVENC Encode 결과를 저장하기 위한 Output Buffer 생성
	if (!m_encoderHandle)
	{
		printf_s("[NVENC ERROR] Encoder handle is not initialized.\n");
		return false;
	}

	m_bitstreamBuffers = new (std::nothrow) NV_ENC_OUTPUT_PTR[m_encodeSlotCount]{};
	if (!m_bitstreamBuffers)
		return false;

	for (uint32_t i = 0; i < m_encodeSlotCount; i++)
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

	for (uint32_t i = 0; i < m_encodeSlotCount; i++)
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

bool D3D11NvEncoder_Impl::CreateRegisteredResources()
{
	// NVENC 내부에서 관리하는 Registered 리소스 핸들을 저장할 공간을 만든다.
	// NVENC 가 접근하기 위해서는 Encode 호출 전 사전에 미리 Registered 되어야 한다.
	m_registeredResources = new (std::nothrow) NV_ENC_REGISTERED_PTR[m_encodeSlotCount]{};
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

	for (uint32_t i = 0; i < m_encodeSlotCount; i++)
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

bool D3D11NvEncoder_Impl::CreateD3D11InputBuffers()
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
	m_bgraTextures = new (std::nothrow) ID3D11Texture2D * [m_encodeSlotCount] {};
	if (!m_bgraTextures)
		return false;

	// BGRA->NV12 변환 결과를 저장하기 위한 Output NV12 버퍼
	m_nv12Textures = new (std::nothrow) ID3D11Texture2D * [m_encodeSlotCount] {};
	if (!m_nv12Textures)
	{
		// 앞서 할당한 BGRA 배열을 해제하지 않으면 누수한다.
		DestroyD3D11InputBuffers();
		return false;
	}

	HRESULT hr = S_OK;

	// BGRA 타입 D3D11Texture2D 생성
	for (uint32_t i = 0; i < m_encodeSlotCount; i++)
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
	for (uint32_t i = 0; i < m_encodeSlotCount; i++)
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
	void** inputFrames = new (std::nothrow) void* [m_encodeSlotCount] {};
	if (!inputFrames)
	{
		DestroyD3D11InputBuffers();
		return false;
	}

	// D3D11Texture 의 주소만 void* 캐스팅 해서 저장
	for (uint32_t i = 0; i < m_encodeSlotCount; i++)
	{
		inputFrames[i] = m_nv12Textures[i];
	}

	// NVENC InputResource 로 등록
	if (!RegisterInputResources(inputFrames, m_encodeSlotCount, NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX,
		GetMaxEncodeWidth(), GetMaxEncodeHeight(), GetMaxEncodeWidth(), GetPixelFormat()))
	{
		delete[] inputFrames;
		DestroyD3D11InputBuffers();
		DestroyRegisteredResources();
		CreateRegisteredResources();
		return false;
	}

	// D3D11VideoProcessorNV12 에게 BGRA -> NV12 변환 입력을 저장하게될 버퍼로 설정
	if (!SetBGRAInputTexture(m_bgraTextures, m_encodeSlotCount))
	{
		delete[] inputFrames;
		DestroyD3D11InputBuffers();
		DestroyRegisteredResources();
		CreateRegisteredResources();
		return false;
	}

	// D3D11VideoProcessorNV12 에게 BGRA -> NV12 변환 결과를 저장하게될 버퍼로 설정
	if (!SetNV12OutputTexture(m_nv12Textures, m_encodeSlotCount))
	{
		delete[] inputFrames;
		DestroyD3D11InputBuffers();
		DestroyRegisteredResources();
		CreateRegisteredResources();
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
		for (uint32_t i = 0; i < m_encodeSlotCount; i++)
			SafeRelease(m_bgraTextures[i]);

		delete[] m_bgraTextures;
		m_bgraTextures = nullptr;
	}

	if (m_nv12Textures)
	{
		for (uint32_t i = 0; i < m_encodeSlotCount; i++)
			SafeRelease(m_nv12Textures[i]);

		delete[] m_nv12Textures;
		m_nv12Textures = nullptr;
	}
}

bool D3D11NvEncoder_Impl::CreateBGRAToNV12Converter()
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

void D3D11NvEncoder_Impl::DestroyBGRAToNV12Converter()
{
	// BGRA -> NV12 변환 작업을 해주는 Converter 해제
	if (m_converter)
	{
		m_converter->Destroy();
		delete m_converter;
		m_converter = nullptr;
	}
}

bool D3D11NvEncoder_Impl::CreatePacketBuffers()
{
	// Encode 결과를 저장해줄 OutputFrame 생성
	// 버퍼 수량 만큼의 공간만 할당하고 실제 Encode Result 저장할 공간은
	// Bitstream 을 읽어올 때 설정한다.
	m_packetBuffers = new (std::nothrow) NvEncPacketBuffer[m_encodeSlotCount]{};
	return (m_packetBuffers != nullptr);
}

void D3D11NvEncoder_Impl::DestroyPacketBuffers()
{
	// Encode 결과를 저장해줄 OutputFrame 해제

	if (!m_packetBuffers)
	{
		return;
	}

	for (uint32_t i = 0; i < m_encodeSlotCount; i++)
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
	frame.timestamp = 0;
	frame.isKeyFrame = false;
}

bool D3D11NvEncoder_Impl::CreatePendingFrames()
{
	m_pendingFrames = new (std::nothrow) NvEncPendingFrame[m_encodeSlotCount]{};
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

bool D3D11NvEncoder_Impl::CreateEncodeCompletionThread()
{
	// 동기 이벤트는 CreateSyncEvents 가 이미 만들어 두었다.
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

void D3D11NvEncoder_Impl::DestroyEncodeCompletionThread()
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

void D3D11NvEncoder_Impl::Destroy()
{
	// 큐 펌프를 먼저 멈춘다. 이게 사라져야 새 프레임이 들어오지 않는다.
	// 예전에는 앱이 EncodeThread::Shutdown 을 Destroy 보다 먼저 부를 책임을 졌고,
	// 순서를 틀리면 스레드가 해제된 엔코더를 만졌다.
	StopEncodeThread();

	// 소비자가 멈춘 뒤에 큐를 닫는다. Shutdown 이 대기 중이던 프레임을
	// 앱에 돌려주므로 캡처 슬롯이 묶인 채 남지 않는다.
	DestroyInputQueue();

	::InterlockedExchange(&m_acceptFrames, FALSE);

	if (m_encodeCompletionThread)
	{
		DestroyEncodeCompletionThread();
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
		DestroyBGRAToNV12Converter();
		DestroyEncoder();
	}

	// 동기 이벤트는 위의 모든 스레드가 정지한 뒤에 닫는다.
	DestroySyncEvents();

	SafeRelease(m_D3D11Context);
	SafeRelease(m_D3D11Device);
	m_contextGate = nullptr;
}

// =============================================================================
// 재설정
// =============================================================================

bool D3D11NvEncoder_Impl::StaticFieldsDiffer(const NvEncConfig& a, const NvEncConfig& b)
{
	return a.width != b.width
		|| a.height != b.height
		|| a.maxWidth != b.maxWidth
		|| a.maxHeight != b.maxHeight
		|| a.encodeSlotCount != b.encodeSlotCount
		|| a.enableAsyncPipeline != b.enableAsyncPipeline
		|| a.latencyMode != b.latencyMode
		|| a.profile != b.profile
		|| a.enableIntraRefresh != b.enableIntraRefresh
		|| a.intraRefreshPeriodFrames != b.intraRefreshPeriodFrames
		|| a.gopLengthFrames != b.gopLengthFrames
		|| a.repeatSequenceHeader != b.repeatSequenceHeader;
}

NvEncReconfigureResult D3D11NvEncoder_Impl::Reconfigure(const NvEncConfig& config, bool forceIdr)
{
	if (!m_encoderHandle || IsFaulted())
		return NvEncReconfigureResult::NotInitialized;

	if (config.averageBitrateBps == 0
		|| config.frameRateNumerator == 0
		|| config.frameRateDenominator == 0)
	{
		printf_s("[NVENC ERROR] Reconfigure rejected: bitrate and frame rate must be non-zero.\n");
		return NvEncReconfigureResult::InvalidConfig;
	}

	// 동시에 들어오는 Reconfigure 를 직렬화하고, m_userConfig 를 일관되게 읽는다.
	::AcquireSRWLockExclusive(&m_configLock);

	if (StaticFieldsDiffer(m_userConfig, config))
	{
		::ReleaseSRWLockExclusive(&m_configLock);
		printf_s("[NVENC ERROR] Reconfigure rejected: an init-only field changed."
			" Resolution, buffer count, latency mode, profile and GOP structure"
			" require Destroy() then Initialize().\n");
		return NvEncReconfigureResult::InitOnlyFieldChanged;
	}

	if (memcmp(&m_userConfig, &config, sizeof(NvEncConfig)) == 0 && !forceIdr)
	{
		::ReleaseSRWLockExclusive(&m_configLock);
		return NvEncReconfigureResult::NoChange;
	}

	ApplyRateControlConfig(config);

	NV_ENC_RECONFIGURE_PARAMS reconfigureParams = {};
	reconfigureParams.version = NV_ENC_RECONFIGURE_PARAMS_VER;
	reconfigureParams.reInitEncodeParams = m_initParameters;
	reconfigureParams.reInitEncodeParams.encodeConfig = &m_config;
	reconfigureParams.resetEncoder = 0;
	reconfigureParams.forceIDR = forceIdr ? 1U : 0U;

	bool applied = false;
	{
		// NVENC 는 내부적으로 D3D11 을 만질 수 있고, 이 호출은 엔코드 스레드의
		// EncodePicture 와 겹칠 수 있다. 게이트로 직렬화한다.
		D3D11ImmediateContextGuard contextGuard(m_contextGate);
		applied = NVENC_API_CALL(
			m_nvenc.nvEncReconfigureEncoder(m_encoderHandle, &reconfigureParams));
	}

	if (!applied)
	{
		// 실패했으면 이전 설정을 파라메터에 되돌려 놓는다.
		ApplyRateControlConfig(m_userConfig);
		::ReleaseSRWLockExclusive(&m_configLock);
		return NvEncReconfigureResult::DriverRejected;
	}

	m_userConfig = config;
	::ReleaseSRWLockExclusive(&m_configLock);

	printf_s("[NVENC] Reconfigured. %u bps, %u/%u fps, vbv %u bits%s\n",
		config.averageBitrateBps, config.frameRateNumerator, config.frameRateDenominator,
		m_config.rcParams.vbvBufferSize, forceIdr ? ", forced IDR" : "");

	return NvEncReconfigureResult::Applied;
}

// =============================================================================
// 공유 입력 풀 (생산자 디바이스 연결)
// =============================================================================

// 생산자가 준 NT 핸들들을 이 디바이스에서 한 번만 연다.
//
// 매 프레임 여는 것이 아니라 여기서 끝내는 것이 요점이다.
// OpenSharedResource1 도 IDXGIKeyedMutex 조회도 드라이버를 타는 호출이라
// 프레임마다 하면 이 구조를 도입한 이유가 사라진다.
bool D3D11NvEncoder_Impl::RegisterSharedInputPool(const HANDLE* sharedHandles, uint32_t count)
{
	UnregisterSharedInputPool();

	if (!sharedHandles || count == 0)
		return false;

	if (!m_D3D11Device)
	{
		printf_s("[NVENC ERROR] RegisterSharedInputPool before Initialize.\n");
		return false;
	}

	// OpenSharedResource1 은 ID3D11Device1 부터다.
	ID3D11Device1* device1 = nullptr;
	HRESULT hr = m_D3D11Device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1));
	if (FAILED(hr) || !device1)
	{
		printf_s("[NVENC ERROR] ID3D11Device1 is not available (hr 0x%08X).\n", static_cast<unsigned int>(hr));
		return false;
	}

	m_sharedInputTextures = new (std::nothrow) ID3D11Texture2D * [count] {};
	m_sharedInputMutexes = new (std::nothrow) IDXGIKeyedMutex * [count] {};
	if (!m_sharedInputTextures || !m_sharedInputMutexes)
	{
		device1->Release();
		UnregisterSharedInputPool();
		return false;
	}

	for (uint32_t i = 0; i < count; ++i)
	{
		if (!sharedHandles[i])
		{
			printf_s("[NVENC ERROR] shared handle %u is null.\n", i);
			device1->Release();
			UnregisterSharedInputPool();
			return false;
		}

		hr = device1->OpenSharedResource1(
			sharedHandles[i], __uuidof(ID3D11Texture2D),
			reinterpret_cast<void**>(&m_sharedInputTextures[i]));
		if (FAILED(hr) || !m_sharedInputTextures[i])
		{
			// 가장 흔한 원인은 두 디바이스가 서로 다른 어댑터인 것이다.
			printf_s("[NVENC ERROR] OpenSharedResource1 failed for slot %u (hr 0x%08X)."
				" are both devices on the same adapter?\n", i, static_cast<unsigned int>(hr));
			device1->Release();
			UnregisterSharedInputPool();
			return false;
		}

		hr = m_sharedInputTextures[i]->QueryInterface(
			__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&m_sharedInputMutexes[i]));
		if (FAILED(hr) || !m_sharedInputMutexes[i])
		{
			printf_s("[NVENC ERROR] the shared texture for slot %u has no keyed mutex"
				" (producer must create it with D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX).\n", i);
			device1->Release();
			UnregisterSharedInputPool();
			return false;
		}
	}

	device1->Release();
	m_sharedInputCount = count;

	printf_s("[NVENC] shared input pool registered : %u slots\n", count);
	return true;
}

void D3D11NvEncoder_Impl::UnregisterSharedInputPool()
{
	if (m_sharedInputMutexes)
	{
		for (uint32_t i = 0; i < m_sharedInputCount; ++i)
			SafeRelease(m_sharedInputMutexes[i]);

		delete[] m_sharedInputMutexes;
		m_sharedInputMutexes = nullptr;
	}

	if (m_sharedInputTextures)
	{
		for (uint32_t i = 0; i < m_sharedInputCount; ++i)
			SafeRelease(m_sharedInputTextures[i]);

		delete[] m_sharedInputTextures;
		m_sharedInputTextures = nullptr;
	}

	m_sharedInputCount = 0;
}

// =============================================================================
// 인코드 스레드 제어
// =============================================================================

bool D3D11NvEncoder_Impl::StartEncodeThread()
{
	// 큐는 Initialize 가 만든다. 없다는 것은 동기 파이프라인이라는 뜻이고,
	// 그때는 아무도 출력을 회수하지 않아 이 스레드가 곧 정지한다.
	if (!m_inputQueue)
	{
		printf_s("[NVENC ERROR] StartEncodeThread requires the async pipeline.\n");
		return false;
	}

	StopEncodeThread();

	m_encodeThread = new (std::nothrow) EncodeThread();
	if (!m_encodeThread)
	{
		return false;
	}

	if (!m_encodeThread->Initialize(m_inputQueue, this))
	{
		delete m_encodeThread;
		m_encodeThread = nullptr;
		return false;
	}

	return true;
}

void D3D11NvEncoder_Impl::StopEncodeThread()
{
	if (!m_encodeThread)
	{
		return;
	}

	// 소멸자가 Shutdown 을 부르지만, 통계를 마지막으로 한 번 걷어둔다.
	m_encodeThread->Shutdown();
	delete m_encodeThread;
	m_encodeThread = nullptr;
}

bool D3D11NvEncoder_Impl::CreateInputQueue(uint32_t depth)
{
	DestroyInputQueue();

	m_inputQueue = new (std::nothrow) EncodeFrameQueue();
	if (!m_inputQueue)
		return false;

	// 큐에는 우리 정적 함수를 걸어 둔다. 앱 콜백은 언제든 갈아끼울 수 있어야
	// 하는데, 큐의 콜백은 Initialize 시점에 고정되기 때문이다.
	if (!m_inputQueue->Initialize(depth, QueueFrameReleaseCallback, this))
	{
		DestroyInputQueue();
		return false;
	}

	return true;
}

void D3D11NvEncoder_Impl::DestroyInputQueue()
{
	if (!m_inputQueue)
		return;

	// Shutdown 이 대기 중인 프레임을 앱에 돌려준다. 그 전에 인코드 스레드가
	// 멈춰 있어야 HELD 슬롯이 남지 않는다 - Destroy 가 그 순서를 지킨다.
	m_inputQueue->Shutdown();
	delete m_inputQueue;
	m_inputQueue = nullptr;
}

void D3D11NvEncoder_Impl::QueueFrameReleaseCallback(NvEncInputFrame& frame, void* userData)
{
	D3D11NvEncoder_Impl* self = static_cast<D3D11NvEncoder_Impl*>(userData);
	if (!self)
		return;

	const NvEncFrameReleaseCallback callback = self->m_frameReleaseCallback;
	void* const callbackUserData = self->m_frameReleaseUserData;

	if (callback)
		callback(frame, callbackUserData);
}

void D3D11NvEncoder_Impl::SetFrameReleaseCallback(NvEncFrameReleaseCallback callback, void* userData)
{
	m_frameReleaseUserData = userData;
	::MemoryBarrier();
	m_frameReleaseCallback = callback;
}

bool D3D11NvEncoder_Impl::EnqueueFrame(const NvEncInputFrame& frame, bool forceKeyFrame)
{
	if (!m_inputQueue)
		return false;

	return m_inputQueue->EnqueueFrame(frame, forceKeyFrame);
}

// =============================================================================
// 콜백 등록
// =============================================================================

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

void D3D11NvEncoder_Impl::SetKeyFrameRequestCallback(bool (*callback)(void*), void* userData)
{
	if (m_encodeThread)
	{
		m_encodeThread->SetKeyFrameRequestCallback(callback, userData);
	}
}

// =============================================================================
// 프레임 투입
// =============================================================================

bool D3D11NvEncoder_Impl::StageFrame(ID3D11Texture2D* bgraTexture)
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

// StageFrame 와 하는 일은 같다. 다른 점은 원본이 다른 디바이스가
// 만든 공유 텍스처라서 keyed mutex 를 잡아야 한다는 것뿐이다.
//
// 뮤텍스를 잡고 있는 구간은 CopyResource 하나다. 그게 끝나면 이 디바이스
// 안에 BGRA 사본이 생기므로 공유 텍스처는 바로 놓는다. 뒤에 오는 NV12
// 변환과 nvEncMapInputResource / nvEncEncodePicture 는 전부 이 디바이스
// 안의 일이고, 그것들이 드라이버 안에서 얼마나 오래 블로킹하든 생산자
// 쪽은 아무 영향을 받지 않는다. 이 구조의 전부가 그 한 줄에 있다.
bool D3D11NvEncoder_Impl::StageFrameFromSharedSlot(uint32_t slot)
{
	if (!m_encoderHandle)
	{
		printf_s("[NVENC ERROR] Encoder handle is not initialized.\n");
		return false;
	}

	if (!m_sharedInputTextures || !m_sharedInputMutexes || slot >= m_sharedInputCount)
	{
		printf_s("[NVENC ERROR] shared input slot %u is out of range (count %u).\n", slot, m_sharedInputCount);
		return false;
	}

	if (!CanSubmitFrame())
		return false;

	if (!m_D3D11Context)
	{
		printf_s("[D3D11 Context ERROR] D3D11 Context is not initialized.\n");
		return false;
	}

	ID3D11Texture2D* sourceTexture = m_sharedInputTextures[slot];
	IDXGIKeyedMutex* keyedMutex = m_sharedInputMutexes[slot];
	if (!sourceTexture || !keyedMutex)
		return false;

	const uint32_t inputSlot = GetInputSlotIndex();
	ID3D11Texture2D* dstTexture = m_bgraTextures[inputSlot];

	// 타임아웃은 유한하다. 생산자가 이 슬롯을 놓지 않았으면 이 프레임을
	// 버린다 — 기다리면 이 구조를 도입한 이유가 사라진다.
	const HRESULT acquireResult = keyedMutex->AcquireSync(
		NVENC_SHARED_INPUT_MUTEX_KEY, NVENC_SHARED_INPUT_MUTEX_TIMEOUT_MS);
	if (acquireResult != S_OK)
		return false;

	{
		D3D11ImmediateContextGuard contextGuard(m_contextGate);
		m_D3D11Context->CopyResource(dstTexture, sourceTexture);

		// 복사를 GPU 로 밀어 둔다. 여기서 밀지 않으면 뮤텍스를 놓은 뒤에
		// 생산자가 원본을 덮어쓰기 시작하는데 우리 복사는 아직 큐에만
		// 있을 수 있다.
		m_D3D11Context->Flush();
	}

	keyedMutex->ReleaseSync(NVENC_SHARED_INPUT_MUTEX_KEY);

	return true;
}

void D3D11NvEncoder_Impl::RequestKeyFrame()
{
	::InterlockedExchange(&m_forceKeyFrame, TRUE);
}

bool D3D11NvEncoder_Impl::SubmitFrame(uint64_t frameId)
{
	if (!CanSubmitFrame())
		return false;

	const uint32_t inputSlot = GetInputSlotIndex();
	NvEncPendingFrame& pendingFrame = m_pendingFrames[inputSlot];
	if (::ReadAcquire(&pendingFrame.submitted) == TRUE)
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

	// 완료 스레드 객체 포인터를 만지지 않는다. 그 포인터는 DestroyEncodeCompletionThread
	// 가 지우므로 여기서 역참조하면 UAF 창이 생긴다. 이벤트는 엔코더 수명 전체 유효.
	if (m_frameSubmittedEvent)
		::SetEvent(m_frameSubmittedEvent);
	return true;
}

bool D3D11NvEncoder_Impl::EncodeSync(NvEncPacket& encodeResultPacket)
{
	if (m_asyncPipelineEnabled)
		return false;

	if (!SubmitFrame(0))
		return false;

	// 슬롯 회수와 실패 복구는 CompleteOldestFrame 에 한 곳으로 모아둔다.
	return CompleteOldestFrame(true, false, &encodeResultPacket) == NvEncCompletionResult::Completed;
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

// =============================================================================
// 인코드 파이프라인 (내부)
// =============================================================================

bool D3D11NvEncoder_Impl::MapInputResource(uint32_t slot)
{
	// EncodeFrame 가 호출 되기 전에 Input Resource Map 수행
	if (!m_registeredResources || !m_mappedInputBuffers || slot >= m_encodeSlotCount)
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
	if (!m_mappedInputBuffers || slot >= m_encodeSlotCount)
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

bool D3D11NvEncoder_Impl::EncodePicture(uint32_t slot)
{
	// 사전에 Registered 된 Input Resource 의 Texture 를 Encode 한다.
	// 여기서의 Input 은 NV12 타입일 것이고, Output 은 H264 로 Encode 된 Bitstream Buffer 이다.
	if (!m_encoderHandle || !m_mappedInputBuffers || !m_bitstreamBuffers || slot >= m_encodeSlotCount)
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
	picParams.inputTimeStamp = m_timestamp++;
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
		return NvEncPacketStatus::Ready;
	}

	HANDLE completionEvent = GetCompletionEvent(slot);
	if (!completionEvent)
		return NvEncPacketStatus::Error;

	const DWORD timeoutMilliseconds = block ? 20'000U : 0U;
	const DWORD dwResult = ::WaitForSingleObject(completionEvent, timeoutMilliseconds);

	if (dwResult == WAIT_OBJECT_0)
		return NvEncPacketStatus::Ready;

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
	if (!m_bitstreamBuffers || !m_packetBuffers || slot >= m_encodeSlotCount)
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
	frame.timestamp = lockBitstreamData.outputTimeStamp;
	frame.isKeyFrame = (lockBitstreamData.pictureType == NV_ENC_PIC_TYPE_IDR);

	packet.data = frame.streamData;
	packet.size = frame.streamDataSize;
	packet.timestamp = frame.timestamp;
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
			const NvEncCompletionResult result = CompleteOldestFrame(true, false);
			if (result == NvEncCompletionResult::Fatal || result == NvEncCompletionResult::NotReady)
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

NvEncCompletionResult D3D11NvEncoder_Impl::CompleteOldestFrame(bool block, bool invokeCallback, NvEncPacket* outPacket)
{
	if (!m_encoderHandle || !m_pendingFrames || GetPendingFrameCount() == 0)
		return NvEncCompletionResult::NotReady;

	const uint32_t outputSlot = GetOutputSlotIndex();
	NvEncPendingFrame& pendingFrame = m_pendingFrames[outputSlot];

	// SubmitFrame 은 submitted = TRUE 를 기록한 뒤에 pending 을 올린다.
	// 따라서 pending > 0 인데 여기서 FALSE 가 보이면 링 장부가 깨진 것이고
	// 어느 슬롯을 회수해야 하는지 알 수 없으므로 세션을 포기한다.
	if (::ReadAcquire(&pendingFrame.submitted) != TRUE)
	{
		printf_s("[NVENC ERROR] Pending frame ring is inconsistent. slot=%u pending=%u\n",
			outputSlot, GetPendingFrameCount());
		EnterFaultedState(NvEncErrorCode::SlotRingCorrupted);
		return NvEncCompletionResult::Fatal;
	}

	const NvEncPacketStatus completionStatus = WaitForEncodeCompletion(outputSlot, block);

	// 논블로킹 폴링에서 아직 안 끝난 경우. 슬롯을 그대로 유지한다.
	if (completionStatus == NvEncPacketStatus::NotReady)
		return NvEncCompletionResult::NotReady;

	// completion event 를 못 받았으면 NVENC 가 아직 이 슬롯의 입력 리소스를
	// 잡고 있을 수 있다. Unmap 도 슬롯 재사용도 안전하지 않으므로 복구하지 않는다.
	if (completionStatus != NvEncPacketStatus::Ready)
	{
		EnterFaultedState(NvEncErrorCode::OutputTimeout);
		return NvEncCompletionResult::Fatal;
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
		return NvEncCompletionResult::Fatal;
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
		return NvEncCompletionResult::FrameLost;
	}

	::InterlockedIncrement64(&m_completedFrameCount);

	if (invokeCallback)
		InvokeEncodedPacketCallback(packet);

	return NvEncCompletionResult::Completed;
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
		for (uint32_t i = 0; i < m_encodeSlotCount; ++i)
		{
			if (::InterlockedExchange(&m_pendingFrames[i].submitted, FALSE) == TRUE)
				::InterlockedIncrement64(&m_lostFrameCount);

			m_pendingFrames[i].frameId = 0;
		}
	}

	::InterlockedExchange(&m_pendingFrameCount, 0);
	SignalAllSlotsFree();
}

void D3D11NvEncoder_Impl::SignalAllSlotsFree()
{
	if (m_allSlotsFreeEvent)
		::SetEvent(m_allSlotsFreeEvent);
}

// =============================================================================
// 입력 리소스 등록
// =============================================================================

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

// =============================================================================
// 오류 / 콜백 통지
// =============================================================================

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

// =============================================================================
// 통계 / 진단
// =============================================================================

void D3D11NvEncoder_Impl::GetStats(NvEncStats& stats) const
{
	stats.submittedFrames = static_cast<uint64_t>(
		::ReadAcquire64(&m_submittedFrameCount));
	stats.completedFrames = static_cast<uint64_t>(
		::ReadAcquire64(&m_completedFrameCount));
	stats.lostFrames = static_cast<uint64_t>(
		::ReadAcquire64(&m_lostFrameCount));
	stats.pendingFrames = GetPendingFrameCount();
	stats.faulted = IsFaulted();

	// 큐 펌프를 쓰지 않으면 이 값들은 0 으로 남는다.
	if (m_inputQueue)
	{
		stats.droppedInputQueue = m_inputQueue->GetDropCount();
		stats.dequeuedFrames = m_inputQueue->GetDequeuedCount();
	}

	if (m_encodeThread)
	{
		m_encodeThread->FillStats(stats);
	}
}

void D3D11NvEncoder_Impl::DebugFailNextOutputs(uint32_t count)
{
	::InterlockedExchange(&m_debugFailOutputCount, static_cast<LONG>(count));
}

bool D3D11NvEncoder_Impl::ConsumeDebugOutputFailure()
{
	// 테스트 훅. 남은 횟수가 있으면 하나 소비하고 실패를 지시한다.
	LONG remaining = ::ReadAcquire(&m_debugFailOutputCount);
	while (remaining > 0)
	{
		const LONG previous = ::InterlockedCompareExchange(&m_debugFailOutputCount, remaining - 1, remaining);
		if (previous == remaining)
			return true;
		remaining = previous;
	}

	return false;
}

// =============================================================================
// getter / setter
// =============================================================================

void D3D11NvEncoder_Impl::GetConfig(NvEncConfig& config) const
{
	::AcquireSRWLockShared(&m_configLock);
	config = m_userConfig;
	::ReleaseSRWLockShared(&m_configLock);
}

bool D3D11NvEncoder_Impl::CanSubmitFrame() const
{
	return m_encoderHandle && m_pendingFrames &&
		::ReadAcquire(&m_acceptFrames) == TRUE &&
		GetPendingFrameCount() < m_encodeSlotCount;
}

uint32_t D3D11NvEncoder_Impl::GetPendingFrameCount() const
{
	return static_cast<uint32_t>(::ReadAcquire(&m_pendingFrameCount));
}

bool D3D11NvEncoder_Impl::IsAsyncPipelineEnabled() const
{
	return m_asyncPipelineEnabled;
}

bool D3D11NvEncoder_Impl::IsFaulted() const
{
	return ::ReadAcquire(&m_faulted) == TRUE;
}

uint32_t D3D11NvEncoder_Impl::GetInputSlotIndex() const
{
	return WrapRingIndex(m_inputSequence, m_encodeSlotCount);
}

uint32_t D3D11NvEncoder_Impl::GetOutputSlotIndex() const
{
	return WrapRingIndex(m_outputSequence, m_encodeSlotCount);
}

HANDLE D3D11NvEncoder_Impl::GetCompletionEvent(uint32_t slot)
{
	return m_slotCompletionEvents && slot < m_encodeSlotCount ? m_slotCompletionEvents[slot] : nullptr;
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
