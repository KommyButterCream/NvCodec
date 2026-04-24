#include "pch.h"
#include "D3D11NvEncoder.h"
#include "D3D11VideoProcessorNV12.h"

#include <new>
#include <assert.h> // for assert
#include <stdio.h> // for printf_s, fopen_s, fwrite

using namespace Core::DirectX;

namespace
{
	inline bool IsPowerOfTwo(uint32_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	inline uint32_t WrapRingIndex(uint32_t index, uint32_t bufferCount)
	{
		return index & (bufferCount - 1);
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

	void LogInvalidBufferFormat(NV_ENC_BUFFER_FORMAT eBufferFormat)
	{
		printf_s("[NVENC ERROR] Invalid Buffer format: %d\n", static_cast<int32_t>(eBufferFormat));
	}

	uint32_t GetWidthInBytes(const NV_ENC_BUFFER_FORMAT eBufferFormat, const uint32_t width)
	{
		switch (eBufferFormat) {
		case NV_ENC_BUFFER_FORMAT_NV12:
		case NV_ENC_BUFFER_FORMAT_YV12:
		case NV_ENC_BUFFER_FORMAT_IYUV:
		case NV_ENC_BUFFER_FORMAT_NV16:
		case NV_ENC_BUFFER_FORMAT_YUV444:
			return width;
		case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
		case NV_ENC_BUFFER_FORMAT_P210:
		case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
			return width * 2;
		case NV_ENC_BUFFER_FORMAT_ARGB:
		case NV_ENC_BUFFER_FORMAT_ARGB10:
		case NV_ENC_BUFFER_FORMAT_AYUV:
		case NV_ENC_BUFFER_FORMAT_ABGR:
		case NV_ENC_BUFFER_FORMAT_ABGR10:
			return width * 4;
		default:
			LogInvalidBufferFormat(eBufferFormat);
			return 0;
		}
	}

	uint32_t GetNumChromaPlanes(const NV_ENC_BUFFER_FORMAT eBufferFormat)
	{
		switch (eBufferFormat)
		{
		case NV_ENC_BUFFER_FORMAT_NV12:
		case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
		case NV_ENC_BUFFER_FORMAT_NV16:
		case NV_ENC_BUFFER_FORMAT_P210:
			return 1;
		case NV_ENC_BUFFER_FORMAT_YV12:
		case NV_ENC_BUFFER_FORMAT_IYUV:
		case NV_ENC_BUFFER_FORMAT_YUV444:
		case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
			return 2;
		case NV_ENC_BUFFER_FORMAT_ARGB:
		case NV_ENC_BUFFER_FORMAT_ARGB10:
		case NV_ENC_BUFFER_FORMAT_AYUV:
		case NV_ENC_BUFFER_FORMAT_ABGR:
		case NV_ENC_BUFFER_FORMAT_ABGR10:
			return 0;
		default:
			LogInvalidBufferFormat(eBufferFormat);
			return 0;
		}
	}

	uint32_t GetChromaPitch(const NV_ENC_BUFFER_FORMAT eBufferFormat, const uint32_t lumaPitch)
	{
		switch (eBufferFormat)
		{
		case NV_ENC_BUFFER_FORMAT_NV12:
		case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
		case NV_ENC_BUFFER_FORMAT_NV16:
		case NV_ENC_BUFFER_FORMAT_P210:
		case NV_ENC_BUFFER_FORMAT_YUV444:
		case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
			return lumaPitch;
		case NV_ENC_BUFFER_FORMAT_YV12:
		case NV_ENC_BUFFER_FORMAT_IYUV:
			return (lumaPitch + 1) / 2;
		case NV_ENC_BUFFER_FORMAT_ARGB:
		case NV_ENC_BUFFER_FORMAT_ARGB10:
		case NV_ENC_BUFFER_FORMAT_AYUV:
		case NV_ENC_BUFFER_FORMAT_ABGR:
		case NV_ENC_BUFFER_FORMAT_ABGR10:
			return 0;
		default:
			LogInvalidBufferFormat(eBufferFormat);
			return 0;
		}
	}

	uint32_t GetChromaHeight(const NV_ENC_BUFFER_FORMAT eBufferFormat, const uint32_t lumaHeight)
	{
		switch (eBufferFormat)
		{
		case NV_ENC_BUFFER_FORMAT_YV12:
		case NV_ENC_BUFFER_FORMAT_IYUV:
		case NV_ENC_BUFFER_FORMAT_NV12:
		case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
			return (lumaHeight + 1) / 2;
		case NV_ENC_BUFFER_FORMAT_NV16:
		case NV_ENC_BUFFER_FORMAT_P210:
			return lumaHeight;
		case NV_ENC_BUFFER_FORMAT_YUV444:
		case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
			return lumaHeight;
		case NV_ENC_BUFFER_FORMAT_ARGB:
		case NV_ENC_BUFFER_FORMAT_ARGB10:
		case NV_ENC_BUFFER_FORMAT_AYUV:
		case NV_ENC_BUFFER_FORMAT_ABGR:
		case NV_ENC_BUFFER_FORMAT_ABGR10:
			return 0;
		default:
			LogInvalidBufferFormat(eBufferFormat);
			return 0;
		}
	}

	uint32_t GetChromaWidthInBytes(const NV_ENC_BUFFER_FORMAT eBufferFormat, const uint32_t lumaWidth)
	{
		switch (eBufferFormat)
		{
		case NV_ENC_BUFFER_FORMAT_YV12:
		case NV_ENC_BUFFER_FORMAT_IYUV:
			return (lumaWidth + 1) / 2;
		case NV_ENC_BUFFER_FORMAT_NV12:
			return lumaWidth;
		case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
			return 2 * lumaWidth;
		case NV_ENC_BUFFER_FORMAT_NV16:
			return lumaWidth;
		case NV_ENC_BUFFER_FORMAT_P210:
			return 2 * lumaWidth;
		case NV_ENC_BUFFER_FORMAT_YUV444:
			return lumaWidth;
		case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
			return 2 * lumaWidth;
		case NV_ENC_BUFFER_FORMAT_ARGB:
		case NV_ENC_BUFFER_FORMAT_ARGB10:
		case NV_ENC_BUFFER_FORMAT_AYUV:
		case NV_ENC_BUFFER_FORMAT_ABGR:
		case NV_ENC_BUFFER_FORMAT_ABGR10:
			return 0;
		default:
			LogInvalidBufferFormat(eBufferFormat);
			return 0;
		}
	}

	uint32_t GetChromaSubPlaneOffsets(const NV_ENC_BUFFER_FORMAT eBufferFormat, const uint32_t pitch, const uint32_t height, uint32_t chromaOffsets[2])
	{
		chromaOffsets[0] = 0;
		chromaOffsets[1] = 0;
		switch (eBufferFormat)
		{
		case NV_ENC_BUFFER_FORMAT_NV12:
		case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
		case NV_ENC_BUFFER_FORMAT_NV16:
		case NV_ENC_BUFFER_FORMAT_P210:
			chromaOffsets[0] = pitch * height;
			return 1;
		case NV_ENC_BUFFER_FORMAT_YV12:
		case NV_ENC_BUFFER_FORMAT_IYUV:
			chromaOffsets[0] = pitch * height;
			chromaOffsets[1] = chromaOffsets[0] + (GetChromaPitch(eBufferFormat, pitch) * GetChromaHeight(eBufferFormat, height));
			return 2;
		case NV_ENC_BUFFER_FORMAT_YUV444:
		case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
			chromaOffsets[0] = pitch * height;
			chromaOffsets[1] = chromaOffsets[0] + (pitch * height);
			return 2;
		case NV_ENC_BUFFER_FORMAT_ARGB:
		case NV_ENC_BUFFER_FORMAT_ARGB10:
		case NV_ENC_BUFFER_FORMAT_AYUV:
		case NV_ENC_BUFFER_FORMAT_ABGR:
		case NV_ENC_BUFFER_FORMAT_ABGR10:
			return 0;
		default:
			LogInvalidBufferFormat(eBufferFormat);
			return 0;
		}
	}
}

D3D11NvEncoder::~D3D11NvEncoder()
{
	Destroy();
}

bool D3D11NvEncoder::Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t encodeBufferCount)
{
	if (!device)
	{
		return false;
	}

	// 건네 받은 D3D11 Device, Context 포인터의 참조 횟수 증가
	// Destroy 시점에 Release 호출 필요
	m_D3D11Device = device;
	m_D3D11Device->AddRef();
	m_D3D11Device->GetImmediateContext(&m_D3D11Context);

	m_width = width;
	m_height = height;
	m_encodeBufferCount = encodeBufferCount;

	assert(IsPowerOfTwo(m_encodeBufferCount) && "encodeBufferCount must be a power of two");
	if (!IsPowerOfTwo(m_encodeBufferCount))
	{
		return false;
	}


	// 단계별 초기화 진행
	// 실패 시 goto 로 정리 순서 보장

	if (!LoadNvEncApi())
		return false;

	if (!OpenEncodeSession())
		return false;

	if (!InitializeEncoder())
		goto fail_encoder;

	if (!InitializeBGRAtoNV12Converter())
		goto fail_converter;

	if (!InitializeAsyncEvent())
		goto fail_async_event;

	if (!InitializeBitstreamBuffers())
		goto fail_bitstream;

	if (!InitializeRegisteredResources())
		goto fail_registered_resources;

	if (!InitializeD3D11InputBuffers())
		goto fail_input_buffers;

	if (!InitializeMappedInputBuffers())
		goto fail_mapped_inputs;

	if (!InitializeOutputFrameBuffers())
		goto fail_output_frames;

	return true;

fail_output_frames:
	DestoryMappedInputBuffers();
fail_mapped_inputs:
	DestoryD3D11InputBuffers();
fail_input_buffers:
	DestroyRegisteredResources();
fail_registered_resources:
	DestoryBitstreamBuffers();
fail_bitstream:
	DestoryAsyncEvent();
fail_async_event:
	DestroyBGRAtoNV12Converter();
fail_converter:
	DestroyEncoder();
fail_encoder:
	return false;
}

void D3D11NvEncoder::Destroy()
{
	// EOS 보내서 Encoder 내부 버퍼를 비워준다.
	if (!Flush())
	{
		printf_s("[NVENC ERROR] Flush failed during Destroy().\n");
	}

	DestroyOutputFrameBuffers();
	DestoryMappedInputBuffers();
	DestroyRegisteredResources();
	DestoryD3D11InputBuffers();
	DestoryBitstreamBuffers();
	DestoryAsyncEvent();
	DestroyBGRAtoNV12Converter();
	DestroyEncoder();

	SafeRelease(m_D3D11Context);
	SafeRelease(m_D3D11Device);
}

bool D3D11NvEncoder::PrepareFrameForEncode(ID3D11Texture2D* bgraTexture)
{
	// Encode 를 수행하기 위한 텍스쳐를 BGRA Texture Pool 에 복사한다.
	if (!m_encoderHandle)
	{
		printf_s("[NVENC ERROR] Encoder handle is not initialized.\n");
		return false;
	}

	if (!m_D3D11Context)
	{
		printf_s("[D3D11 Context ERROR] D3D11 Context is not initialized.\n");
		return false;
	}

	const uint32_t inputFrameIndex = GetNextInputFrameIndex();

	ID3D11Texture2D* dstTexture = m_bgraTextures[inputFrameIndex];

	// GPU -> GPU copy
	m_D3D11Context->CopyResource(dstTexture, bgraTexture);

	return true;
}

bool D3D11NvEncoder::DoEncode(NvEncPacket& encodeResultPacket)
{
	// 실제 Encoding 처리 시퀀스
	// BGRA -> NV12 변환 (D3D11VideoProcessorNV12)
	// Input Resource Map
	// Encode
	// Encode 결과 Bitstream 가져오기
	// Resource Unmap
	// 순서로 Encoding 이 수행 된다.

	const uint32_t inputFrameIndex = GetNextInputFrameIndex();
	const uint32_t outputFrameIndex = GetNextOutputFrameIndex();

	// BGRA -> NV12 변환 (D3D11VideoProcessorNV12)
	if (!m_converter || !m_converter->Convert(inputFrameIndex))
		return false;

	//m_converter->SaveNV12ToFile(outputFrameIndex, "../convert.nv12");

	// Input Resource Map
	if (!MapInputResources(inputFrameIndex))
		return false;

	// Encode
	// NVENC 는 내부 버퍼링 때문에 Encode 결과가 바로 안나올 수 도 있다.
	// 그런 경우 needsMoreInput 플래그가 설정된다.
	bool needsMoreInput = false;
	if (!EncodeFrame(inputFrameIndex, needsMoreInput))
	{
		UnmapInputResources(inputFrameIndex);
		return false;
	}

	if (inputFrameIndex != outputFrameIndex)
	{
		__debugbreak();
	}
	// Encode 결과 Bitstream 가져오기
	// Encode 결과가 준비된 경우에만 가져오도록 한다.
	if (!needsMoreInput)
	{
		if (!GetEncodedPacket(outputFrameIndex, encodeResultPacket))
		{
			UnmapInputResources(inputFrameIndex);
			return false;
		}
		m_outputFrameIndex++;
	}

	m_inputFrameIndex++;

	// Resource Unmap
	if (!UnmapInputResources(inputFrameIndex))
		return false;

	return true;
}

bool D3D11NvEncoder::LoadNvEncApi()
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

bool D3D11NvEncoder::OpenEncodeSession()
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

bool D3D11NvEncoder::InitializeEncoder()
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
	m_initParameters.enableEncodeAsync = GetCapabilityValue(NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS::NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT);
	m_initParameters.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
	m_initParameters.tuningInfo = NV_ENC_TUNING_INFO::NV_ENC_TUNING_INFO_LOW_LATENCY;

	// 위에서 설정된 파라메터로 NVENC Encoder 생성
	return NVENC_API_CALL(m_nvenc.nvEncInitializeEncoder(m_encoderHandle, &m_initParameters));
}

void D3D11NvEncoder::DestroyEncoder()
{
	// NVENC Encoder 리소스를 해제한다.
	if (!m_encoderHandle)
	{
		return;
	}

	NVENC_API_CALL(m_nvenc.nvEncDestroyEncoder(m_encoderHandle));
	m_encoderHandle = nullptr;
}

bool D3D11NvEncoder::InitializeAsyncEvent()
{
	// 비동기로 처리되는 NVENC Encode 완료 이벤트를 통지 받기 위한
	// 이벤트를 생성 후 NVENC 에 Register 한다.

	if (!m_encoderHandle)
	{
		printf_s("[NVENC ERROR] Encoder handle is not initialized.\n");
		return false;
	}

	// Async Event Create & Register
	m_completionEvent = new (std::nothrow) HANDLE[m_encodeBufferCount]{};
	if (!m_completionEvent)
		return false;

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		// 이벤트 생성
		m_completionEvent[i] = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!m_completionEvent[i])
		{
			DestoryAsyncEvent();
			return false;
		}

		// NVENC 등록
		NV_ENC_EVENT_PARAMS eventParams = { NV_ENC_EVENT_PARAMS_VER };
		eventParams.completionEvent = m_completionEvent[i];
		if (!NVENC_API_CALL(m_nvenc.nvEncRegisterAsyncEvent(m_encoderHandle, &eventParams)))
		{
			DestoryAsyncEvent();
			return false;
		}
	}

	return true;
}

void D3D11NvEncoder::DestoryAsyncEvent()
{
	// 종료 시점에 등록 되어있던 이벤트를 Unregister 하고
	// 이벤트 핸들을 삭제한다.

	if (!m_encoderHandle)
	{
		return;
	}

	if (!m_completionEvent)
	{
		return;
	}

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		HANDLE& completionEvent = m_completionEvent[i];
		if (completionEvent)
		{
			NV_ENC_EVENT_PARAMS eventParams = { NV_ENC_EVENT_PARAMS_VER };
			eventParams.completionEvent = completionEvent;
			NVENC_API_CALL(m_nvenc.nvEncUnregisterAsyncEvent(m_encoderHandle, &eventParams));

			::CloseHandle(completionEvent);
			completionEvent = nullptr;
		}
	}

	delete[] m_completionEvent;
	m_completionEvent = nullptr;
}

bool D3D11NvEncoder::InitializeMappedInputBuffers()
{
	// NVENC Encoding 을 위한 Input Buffer 를 미리 생성한다.
	// Encode 수행 함수를 호출할때 NV_ENC_INPUT_PTR 타입 필요.
	m_mappedInputBuffers = new (std::nothrow) NV_ENC_INPUT_PTR[m_encodeBufferCount]{};
	return (m_mappedInputBuffers != nullptr);
}

void D3D11NvEncoder::DestoryMappedInputBuffers()
{
	// Input Buffer 해제
	// Map 되어 있는 리소스가 있다면 해제 해준다.
	if (!m_encoderHandle)
	{
		return;
	}

	if (!m_mappedInputBuffers)
	{
		return;
	}

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		NV_ENC_INPUT_PTR& mappedInputBuffer = m_mappedInputBuffers[i];
		if (mappedInputBuffer)
		{
			NVENC_API_CALL(m_nvenc.nvEncUnmapInputResource(m_encoderHandle, mappedInputBuffer));
			mappedInputBuffer = nullptr;
		}
	}

	delete[] m_mappedInputBuffers;
	m_mappedInputBuffers = nullptr;
}

bool D3D11NvEncoder::InitializeBitstreamBuffers()
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
			DestoryBitstreamBuffers();
			return false;
		}
		m_bitstreamBuffers[i] = bitstreamBufferParams.bitstreamBuffer;
	}

	return true;
}

void D3D11NvEncoder::DestoryBitstreamBuffers()
{
	// Output Buffer 리소스 해제

	if (!m_encoderHandle)
	{
		return;
	}

	if (!m_bitstreamBuffers)
	{
		return;
	}

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		NV_ENC_OUTPUT_PTR& bitstreamBuffer = m_bitstreamBuffers[i];
		if (bitstreamBuffer)
		{
			NVENC_API_CALL(m_nvenc.nvEncDestroyBitstreamBuffer(m_encoderHandle, bitstreamBuffer));
			bitstreamBuffer = nullptr;
		}
	}

	delete[] m_bitstreamBuffers;
	m_bitstreamBuffers = nullptr;
}

bool D3D11NvEncoder::InitializeRegisteredResources()
{
	// NVENC 내부에서 관리하는 Registered 리소스를 저장하기 위한 리소스 핸들과 프레임 메타데이터를 생성
	// NVENC 가 접근하기 위해서는 Encode 호출 전 사전에 미리 Registered 되어야 한다.
	m_registeredResources = new (std::nothrow) NV_ENC_REGISTERED_PTR[m_encodeBufferCount]{};
	m_inputFrames = new (std::nothrow) NvEncInputFrame[m_encodeBufferCount]{};
	if (!m_registeredResources || !m_inputFrames)
	{
		delete[] m_registeredResources;
		m_registeredResources = nullptr;
		delete[] m_inputFrames;
		m_inputFrames = nullptr;
		return false;
	}

	return true;
}

void D3D11NvEncoder::DestroyRegisteredResources()
{
	// NVENC 내부에서 관리하는 Registered 리소스 해제
	if (!m_encoderHandle)
	{
		return;
	}

	if (!m_registeredResources)
	{
		return;
	}

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		NV_ENC_REGISTERED_PTR& registeredResource = m_registeredResources[i];
		if (registeredResource)
		{
			NVENC_API_CALL(m_nvenc.nvEncUnregisterResource(m_encoderHandle, registeredResource));
			registeredResource = nullptr;
		}
	}

	delete[] m_registeredResources;
	m_registeredResources = nullptr;
}

bool D3D11NvEncoder::InitializeD3D11InputBuffers()
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

	// BGRA->NV12 변환 결과를 저장하기 위한 Output BGRA 버퍼
	m_nv12Textures = new (std::nothrow) ID3D11Texture2D * [m_encodeBufferCount] {};
	if (!m_nv12Textures)
		return false;

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
			DestoryD3D11InputBuffers();
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
			DestoryD3D11InputBuffers();
			return false;
		}
	}

	// NVENC API 는 void* 기반이라 캐스팅을 위한 임시 inputFrames 버퍼 생성
	void** inputFrames = new (std::nothrow) void* [m_encodeBufferCount] {};
	if (!inputFrames)
	{
		DestoryD3D11InputBuffers();
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
		DestoryD3D11InputBuffers();
		DestroyRegisteredResources();
		InitializeRegisteredResources();
		return false;
	}

	// D3D11VideoProcessorNV12 에게 BGRA -> NV12 변환 입력을 저장하게될 버퍼로 설정
	if (!SetBGRAInputTexture(m_bgraTextures, m_encodeBufferCount))
	{
		delete[] inputFrames;
		DestoryD3D11InputBuffers();
		DestroyRegisteredResources();
		InitializeRegisteredResources();
		return false;
	}

	// D3D11VideoProcessorNV12 에게 BGRA -> NV12 변환 결과를 저장하게될 버퍼로 설정
	if (!SetNV12OutputTexture(m_nv12Textures, m_encodeBufferCount))
	{
		delete[] inputFrames;
		DestoryD3D11InputBuffers();
		DestroyRegisteredResources();
		InitializeRegisteredResources();
		return false;
	}

	delete[] inputFrames;
	return true;
}

void D3D11NvEncoder::DestoryD3D11InputBuffers()
{
	// NVENC Encode 를 위한 리소스 해제

	if (!m_bgraTextures)
	{
		delete[] m_inputFrames;
		m_inputFrames = nullptr;
		return;
	}

	if (!m_nv12Textures)
	{
		delete[] m_inputFrames;
		m_inputFrames = nullptr;
		return;
	}

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		SafeRelease(m_bgraTextures[i]);
		SafeRelease(m_nv12Textures[i]);
	}

	delete[] m_bgraTextures;
	m_bgraTextures = nullptr;

	delete[] m_nv12Textures;
	m_nv12Textures = nullptr;

	delete[] m_inputFrames;
	m_inputFrames = nullptr;
}

bool D3D11NvEncoder::InitializeBGRAtoNV12Converter()
{
	// BGRA -> NV12 변환 작업을 해주는 Converter 생성 및 초기화
	m_converter = new (std::nothrow) D3D11VideoProcessorNV12();
	if (!m_converter)
		return false;

	bool result = m_converter->Initialize(m_D3D11Device, m_D3D11Context, m_width, m_height);
	if (!result)
	{
		delete m_converter;
		m_converter = nullptr;
	}

	return result;
}

void D3D11NvEncoder::DestroyBGRAtoNV12Converter()
{
	// BGRA -> NV12 변환 작업을 해주는 Converter 해제
	if (m_converter)
	{
		m_converter->Destory();
		delete m_converter;
		m_converter = nullptr;
	}
}

bool D3D11NvEncoder::InitializeOutputFrameBuffers()
{
	// Encode 결과를 저장해줄 OutputFrame 생성
	// 버퍼 수량 만큼의 공간만 할당하고 실제 Encode Result 저장할 공간은
	// Bitstream 을 읽어올 때 설정한다.
	m_outputFrames = new (std::nothrow) NvEncOutputFrame[m_encodeBufferCount]{};
	return (m_outputFrames != nullptr);
}

void D3D11NvEncoder::DestroyOutputFrameBuffers()
{
	// Encode 결과를 저장해줄 OutputFrame 해제

	if (!m_outputFrames)
	{
		return;
	}

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		ReleaseOutputFrameBuffer(m_outputFrames[i]);
	}

	delete[] m_outputFrames;
	m_outputFrames = nullptr;
}

void D3D11NvEncoder::ReleaseOutputFrameBuffer(NvEncOutputFrame& frame)
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

bool D3D11NvEncoder::RegisterResource(void* buffer, NV_ENC_INPUT_RESOURCE_TYPE eResourceType, uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT eBufferFormat, NV_ENC_BUFFER_USAGE eBufferUsage, NV_ENC_REGISTERED_PTR& registeredResource)
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

bool D3D11NvEncoder::RegisterInputResources(void** inputFrames, uint32_t inputFrameCount, NV_ENC_INPUT_RESOURCE_TYPE eResourceType, uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT eBufferFormat)
{
	// 외부의 D3D11 BGRA Texture 를 넘겨받아 NVENC 가 접근 가능하도록 리소스로 등록한다.
	// inputFrames 는 프로그램 종료 시점까지 해제되지 않으며, 고정된 크기와 수량의 버퍼로 생성되어 있어야 한다.

	if (!inputFrames || !m_registeredResources || !m_inputFrames)
	{
		return false;
	}

	// 외부에서 받은 프레임(D3D11Texture2D BGRA) 를 NVENC 의 Input 으로 Register 수행
	for (uint32_t i = 0; i < inputFrameCount; ++i)
	{
		NV_ENC_REGISTERED_PTR registeredPtr = nullptr;
		if (!RegisterResource(inputFrames[i], eResourceType, width, height, pitch, eBufferFormat, NV_ENC_INPUT_IMAGE, registeredPtr))
		{
			for (uint32_t cleanupIndex = 0; cleanupIndex < i; ++cleanupIndex)
			{
				if (m_registeredResources[cleanupIndex])
				{
					NVENC_API_CALL(m_nvenc.nvEncUnregisterResource(m_encoderHandle, m_registeredResources[cleanupIndex]));
					m_registeredResources[cleanupIndex] = nullptr;
				}
				m_inputFrames[cleanupIndex] = {};
			}
			return false;
		}

		// 등록이 완료된 후 Input Resource 에 대한 핸들곽 메타 데이터 저장
		uint32_t chromaOffsets[2] = { 0, 0 };
		uint32_t chromaPlaneCount = GetChromaSubPlaneOffsets(eBufferFormat, pitch, height, chromaOffsets);
		NvEncInputFrame inputframe = {};
		inputframe.inputPtr = inputFrames[i];
		inputframe.chromaOffsets[0] = chromaOffsets[0];
		inputframe.chromaOffsets[1] = chromaOffsets[1];
		inputframe.numChromaPlanes = GetNumChromaPlanes(eBufferFormat);
		inputframe.pitch = pitch;
		inputframe.chromaPitch = GetChromaPitch(eBufferFormat, pitch);
		inputframe.bufferFormat = eBufferFormat;
		inputframe.resourceType = eResourceType;

		if (chromaPlaneCount == 0)
		{
			inputframe.chromaOffsets[0] = 0;
			inputframe.chromaOffsets[1] = 0;
		}

		m_registeredResources[i] = registeredPtr;
		m_inputFrames[i] = inputframe;
	}

	return true;
}

bool D3D11NvEncoder::SetBGRAInputTexture(ID3D11Texture2D** textures, uint32_t bufferCount)
{
	// 외부의 BGRA D3D11 Texture 를 받아서 컨버터 Input 으로 설정
	if (!m_converter)
		return false;

	return m_converter->SetInputTextures(textures, bufferCount);
}

bool D3D11NvEncoder::SetNV12OutputTexture(ID3D11Texture2D** textures, uint32_t bufferCount)
{
	// 외부의 NV12 D3D11 Texture 를 받아서 컨버터 Output 으로 설정
	if (!m_converter)
		return false;

	return m_converter->SetOutputTextures(textures, bufferCount);
}

bool D3D11NvEncoder::EncodeFrame(uint32_t index, bool& needsMoreInput)
{
	// 사전에 Registered 된 Input Resource 의 Texture 를 Encode 한다.
	// 여기서의 Input 은 NV12 타입일 것이고, Output 은 H264 로 Encode 된 Bitstream Buffer 이다.
	needsMoreInput = false;

	if (!m_encoderHandle || !m_mappedInputBuffers || !m_bitstreamBuffers || index >= m_encodeBufferCount)
	{
		return false;
	}

	// Input, Output 버퍼를 가져와서 Encode Request
	NV_ENC_INPUT_PTR inputBuffer = m_mappedInputBuffers[index];
	NV_ENC_OUTPUT_PTR outputBuffer = m_bitstreamBuffers[index];
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
	picParams.frameIdx = m_inputFrameIndex;
	picParams.outputBitstream = outputBuffer;
	picParams.completionEvent = GetCompletionEvent(index);
	//picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;

	NVENCSTATUS nvStatus = m_nvenc.nvEncEncodePicture(m_encoderHandle, &picParams);
	if (nvStatus == NV_ENC_ERR_NEED_MORE_INPUT)
	{
		needsMoreInput = true;
		return true;
	}

	return NVENC_API_CALL(nvStatus);
}

bool D3D11NvEncoder::WaitForCompletionEvent(uint32_t index)
{
	// Async Encode 를 사용하는 경우
	// Encode 완료 이벤트를 NVENC 내부에서 Set 해준다.
	// 이 Event 를 대기하여 동기를 맞춘다.
	if (m_initParameters.enableEncodeAsync == 0U)
	{
		return true;
	}

	DWORD dwResult = ::WaitForSingleObject(GetCompletionEvent(index), 20'000);

	if (dwResult == WAIT_FAILED)
	{
		printf_s("[NVENC ERROR] Failed to encode frame.\n");
		return false;
	}
	else if (dwResult == WAIT_TIMEOUT)
	{
		printf_s("[NVENC ERROR] Timeout encode frame.\n");
		return false;
	}

	return true;
}

bool D3D11NvEncoder::GetEncodedPacket(uint32_t index, NvEncPacket& packet)
{
	// Encode 완료를 기다리고 H264 로 Encode 된 Bitstream Buffer 를 읽어온다.
	if (!m_bitstreamBuffers || !m_outputFrames || index >= m_encodeBufferCount)
		return false;

	// Encode Complete 대기
	if (!WaitForCompletionEvent(index))
		return false;

	// Encode Result 를 가져오기 위해 NVENC 내부 Bitstream Buffer Lock
	NV_ENC_LOCK_BITSTREAM lockBitstreamData = {};
	lockBitstreamData.version = NV_ENC_LOCK_BITSTREAM_VER;
	lockBitstreamData.outputBitstream = m_bitstreamBuffers[index];
	lockBitstreamData.doNotWait = false;
	if (!NVENC_API_CALL(m_nvenc.nvEncLockBitstream(m_encoderHandle, &lockBitstreamData)))
		return false;

	// Bitstream Result 를 저장하기 위한 frame 획득
	NvEncOutputFrame& frame = m_outputFrames[index];

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

	// Bitstream Buffer Unlock
	return NVENC_API_CALL(m_nvenc.nvEncUnlockBitstream(m_encoderHandle, lockBitstreamData.outputBitstream));
}

bool D3D11NvEncoder::Flush()
{
	// EOS 보내서 Encoder 내부 버퍼를 비워준다.

	if (!m_encoderHandle)
		return true;

	NvEncPacket packet;

	NV_ENC_PIC_PARAMS picParams = {};
	picParams.version = NV_ENC_PIC_PARAMS_VER;
	picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
	picParams.inputPitch = 0;

	if (!NVENC_API_CALL(m_nvenc.nvEncEncodePicture(m_encoderHandle, &picParams)))
		return false;

	for (uint32_t i = 0; i < m_encodeBufferCount; i++)
	{
		try
		{
			if (!GetEncodedPacket(i, packet))
				break;
		}
		catch (...)
		{
			printf_s("[NVENC ERROR] Exception occurred during Flush().\n");
			break;
		}
	}

	return true;
}

const NvEncInputFrame* D3D11NvEncoder::GetNextInputFrame()
{
	uint32_t index = WrapRingIndex(m_inputFrameIndex, m_encodeBufferCount);
	return m_inputFrames ? &m_inputFrames[index] : nullptr;
}

uint32_t D3D11NvEncoder::GetNextInputFrameIndex() const
{
	return WrapRingIndex(m_inputFrameIndex, m_encodeBufferCount);
}

uint32_t D3D11NvEncoder::GetNextOutputFrameIndex() const
{
	return WrapRingIndex(m_outputFrameIndex, m_encodeBufferCount);
}

bool D3D11NvEncoder::MapInputResources(uint32_t index)
{
	// EncodeFrame 가 호출 되기 전에 Input Resource Map 수행
	if (!m_registeredResources || !m_mappedInputBuffers || index >= m_encodeBufferCount)
		return false;

	NV_ENC_MAP_INPUT_RESOURCE mapInputResource = { };

	mapInputResource.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
	mapInputResource.registeredResource = m_registeredResources[index];
	if (!NVENC_API_CALL(m_nvenc.nvEncMapInputResource(m_encoderHandle, &mapInputResource)))
		return false;

	m_mappedInputBuffers[index] = mapInputResource.mappedResource;
	return true;
}

bool D3D11NvEncoder::UnmapInputResources(uint32_t index)
{
	// EncodeFrame 완료된 후 Input Resource Unmap 수행
	if (!m_mappedInputBuffers || index >= m_encodeBufferCount)
		return false;

	if (m_mappedInputBuffers[index])
	{
		if (!NVENC_API_CALL(m_nvenc.nvEncUnmapInputResource(m_encoderHandle, m_mappedInputBuffers[index])))
			return false;

		m_mappedInputBuffers[index] = nullptr;
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

int32_t D3D11NvEncoder::GetCapabilityValue(GUID guidCodec, NV_ENC_CAPS capsToQuery)
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

inline uint32_t D3D11NvEncoder::GetEncodeWidth() const
{
	return m_initParameters.encodeWidth;
}

inline uint32_t D3D11NvEncoder::GetEncodeHeight() const
{
	return  m_initParameters.encodeHeight;
}

uint32_t D3D11NvEncoder::GetMaxEncodeWidth() const
{
	return m_initParameters.maxEncodeWidth;
}

uint32_t D3D11NvEncoder::GetMaxEncodeHeight() const
{
	return m_initParameters.maxEncodeHeight;
}

NV_ENC_BUFFER_FORMAT D3D11NvEncoder::GetPixelFormat() const
{
	return m_initParameters.bufferFormat;
}

DXGI_FORMAT D3D11NvEncoder::GetD3D11Format(NV_ENC_BUFFER_FORMAT eBufferFormat) const
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

HANDLE D3D11NvEncoder::GetCompletionEvent(uint32_t index)
{
	return m_completionEvent ? m_completionEvent[index] : nullptr;
}

