#include "pch.h"
#include "D3D11NvDecoder_Impl.h"

#include <cudaD3D11.h>
#include <malloc.h>
#include <stdio.h> // for printf_s, fopen_s, fwrite
#include <math.h>

#include "ColorSpaceCuda.cuh"

using namespace Core::DirectX;

inline bool CheckCudaDriverAPICall(
	CUresult errorCode,
	const char* func,
	const char* file,
	int32_t line)
{
	if (errorCode == CUDA_SUCCESS)
		return true;

	const char* errName = nullptr;
	cuGetErrorName(errorCode, &errName);

	printf_s("[CUDA ERROR] %s | %s (%s:%d)\n",
		errName ? errName : "UNKNOWN",
		func,
		file,
		line);

	return false;
}

inline bool CheckNvDecodeAPICall(
	CUresult errorCode,
	const char* expr,
	const char* func,
	const char* file,
	int32_t line)
{
	if (errorCode == CUDA_SUCCESS)
		return true;

	const char* errName = nullptr;
	const char* errString = nullptr;

	cuGetErrorName(errorCode, &errName);
	cuGetErrorString(errorCode, &errString);

	printf_s("[NVDEC ERROR]\n");
	printf_s("  API   : %s\n", expr);
	printf_s("  Code  : %d (%s)\n", errorCode, errName ? errName : "UNKNOWN");
	printf_s("  Desc  : %s\n", errString ? errString : "NoDesc");
	printf_s("  Where : %s (%s:%d)\n\n", func, file, line);

	return false;
}

#define CUDA_DRVAPI_CALL(call) \
    CheckCudaDriverAPICall((call), __FUNCTION__, __FILE__, __LINE__)

#define NVDEC_API_CALL(call) \
    CheckNvDecodeAPICall((call), #call, __FUNCTION__, __FILE__, __LINE__)

namespace
{
	// Cuda Context Push/Pop 누락/불일치 방지 하기 위한 RAII Class
	// Cuda Driver API 사용 시 Thread 마다 Current Context 를 직접 관리해야 한다.
	class ScopedCudaContext
	{
	public:
		explicit ScopedCudaContext(CUcontext context)
		{
			m_isActive = (context != nullptr) && CUDA_DRVAPI_CALL(cuCtxPushCurrent(context));
		}

		~ScopedCudaContext()
		{
			if (m_isActive)
			{
				CUDA_DRVAPI_CALL(cuCtxPopCurrent(nullptr));
			}
		}

		bool IsActive() const
		{
			return m_isActive;
		}

	private:
		bool m_isActive = false;
	};

	// 해상도 변경 등의 이유로 Reconfigure 중
	// Display Callback 이 동시에 호출되어 해제된 리소스에 접근하는 것을 방지
	class ScopedReconfigureFlag
	{
	public:
		explicit ScopedReconfigureFlag(volatile LONG* flag)
			: m_flag(flag)
		{
			::InterlockedExchange(m_flag, TRUE);
		}

		~ScopedReconfigureFlag()
		{
			::InterlockedExchange(m_flag, FALSE);
		}

	private:
		volatile LONG* m_flag = nullptr;
	};

	float GetChromaHeightFactor(cudaVideoSurfaceFormat eSurfaceFormat)
	{
		float factor = 0.5f;
		switch (eSurfaceFormat)
		{
		case cudaVideoSurfaceFormat_NV12:
		case cudaVideoSurfaceFormat_P016:
			factor = 0.5f;
			break;
		case cudaVideoSurfaceFormat_YUV444:
		case cudaVideoSurfaceFormat_YUV444_16Bit:
		case cudaVideoSurfaceFormat_NV16:
		case cudaVideoSurfaceFormat_P216:
			factor = 1.0f;
			break;
		}

		return factor;
	}

	uint32_t GetChromaPlaneCount(cudaVideoSurfaceFormat eSurfaceFormat)
	{
		switch (eSurfaceFormat)
		{
		case cudaVideoSurfaceFormat_YUV444:
		case cudaVideoSurfaceFormat_YUV444_16Bit:
			return 2;
		default:
			return 1;
		}
	}
}

D3D11NvDecoder_Impl::D3D11NvDecoder_Impl()
{

}

D3D11NvDecoder_Impl::~D3D11NvDecoder_Impl()
{
	ShutDown();
}

bool D3D11NvDecoder_Impl::Initialize(ID3D11Device* device)
{
	// 디코더를 사용하기 위한 초기화 수행

	if (!device)
	{
		return false;
	}

	// 건네 받은 D3D11 Device, Context 포인터의 참조 횟수 증가
	// Destroy 시점에 Release 호출 필요
	m_D3D11Device = device;
	m_D3D11Device->AddRef();
	m_D3D11Device->GetImmediateContext(&m_D3D11Context);

	// 디코더 생성을 위한 Cuda Driver 초기화
	// Cuda Context 생성/획득
	// Cuda Context 생성/획득
	// NVDEC 사용을 위한 ctxLock, Stream, Event, Parser 리소스 생성
	if (!InitializeCuda())
	{
		ShutDown();
		return false;
	}

	return true;
}

void D3D11NvDecoder_Impl::ShutDown()
{
	// NVDEC Parser 및 CUDA Resource, D3D11 Resource 해제
	if (m_parser)
	{
		NVDEC_API_CALL(cuvidDestroyVideoParser(m_parser));
		m_parser = nullptr;
	}

	if (m_cudaContext)
	{
		ScopedCudaContext cudaContext(m_cudaContext);
		if (cudaContext.IsActive())
		{
			// Decode 수행 중인 프레임이 있다면 전부 기다린 후에 해제한다.
			WaitForAllFrames();
			DestroyTexturePool();
			DestroyCudaDeviceMemoryPool();

			if (m_decoder)
			{
				NVDEC_API_CALL(cuvidDestroyDecoder(m_decoder));
				m_decoder = nullptr;
			}

			for (int32_t i = 0; i < TEXTURE_POOL_COUNT; ++i)
			{
				if (m_cuEvents[i])
				{
					CUDA_DRVAPI_CALL(cuEventDestroy(m_cuEvents[i]));
					m_cuEvents[i] = nullptr;
				}
			}

			if (m_cuStream)
			{
				CUDA_DRVAPI_CALL(cuStreamDestroy(m_cuStream));
				m_cuStream = nullptr;
			}
		}

		if (m_ctxLock)
		{
			NVDEC_API_CALL(cuvidCtxLockDestroy(m_ctxLock));
			m_ctxLock = nullptr;
		}
	}

	if (m_cudaContext)
	{
		CUDA_DRVAPI_CALL(cuDevicePrimaryCtxRelease(m_cudaDevice));
		m_cudaContext = nullptr;
	}

	m_videoFormatDesc = {};
	ZeroMemory(&m_cuVideoFormat, sizeof(m_cuVideoFormat));
	m_writeIndex = 0;
	m_readIndex = 0;
	m_reconfiguring = FALSE;
	m_cacheTextureWidth = 0;
	m_cacheTextureHeight = 0;
	m_cuBGRAPitch = 0;

	if (m_D3D11Context)
	{
		m_D3D11Context->Release();
		m_D3D11Context = nullptr;
	}

	if (m_D3D11Device)
	{
		m_D3D11Device->Release();
		m_D3D11Device = nullptr;
	}
}

bool D3D11NvDecoder_Impl::Parse(const uint8_t* data, uint32_t size, bool endOfPicture, bool endOfStream, bool discontinuity)
{
	// Decode Thread 가 호출하는 Decode Request 함수
	// 이후 HandleVideoSequence -> HandlePictureDecode -> HandlePictureDisplay 순서로 
	// Callback 이 호출된다.
	if (!m_parser || (!data && size > 0))
	{
		return false;
	}

	CUVIDSOURCEDATAPACKET packet = {};
	packet.payload = data;
	packet.payload_size = size;
	packet.flags = CUVID_PKT_TIMESTAMP;

	if (endOfPicture)
		packet.flags |= CUVID_PKT_ENDOFPICTURE;
	if (endOfStream)
		packet.flags |= CUVID_PKT_ENDOFSTREAM;
	if (discontinuity)
		packet.flags |= CUVID_PKT_DISCONTINUITY;

	return NVDEC_API_CALL(cuvidParseVideoData(m_parser, &packet));
}

D3D11NvDecoder_Impl::Frame* D3D11NvDecoder_Impl::GetFrame()
{
	// Decode 결과 D3D11Texture(BGRA) 를 가져가기 위한 외부에 노출된 메서드
	LONG currentWrite = ::InterlockedCompareExchange(&m_writeIndex, 0, 0);
	LONG currentRead = ::InterlockedCompareExchange(&m_readIndex, 0, 0);

	if (currentRead >= currentWrite)
	{
		return nullptr;
	}

	// 3 프레임 이상 밀려 있으면 최신 프레임을 읽어갈 수 있도록 한다.
	const int32_t lag = currentWrite - currentRead;
	if (lag > 3)
	{
		currentRead = currentWrite - 2;
		::InterlockedExchange(&m_readIndex, currentRead);
	}

	// Decode Complete Event 를 수신받은 후 데이터를 읽어간다.
	const int32_t index = currentRead & (TEXTURE_POOL_COUNT - 1);
	if (m_cuEvents[index])
	{
		ScopedCudaContext cudaContext(m_cudaContext);
		if (!cudaContext.IsActive())
		{
			return nullptr;
		}

		// 여기서 Decode Completion 될 때 까지 잠깐 대기한다.
		if (!CUDA_DRVAPI_CALL(cuEventSynchronize(m_cuEvents[index])))
		{
			return nullptr;
		}
	}

	if (!SaveFrameToBmp(index, L"../Decode.bmp"))
	{
		return nullptr;
	}

	::InterlockedIncrement(&m_readIndex);
	return &m_frames[index];
}

int32_t CUDAAPI D3D11NvDecoder_Impl::HandleVideoSequence(void* userData, CUVIDEOFORMAT* format)
{
	return reinterpret_cast<D3D11NvDecoder_Impl*>(userData)->OnVideoSequence(format);
}

int32_t CUDAAPI D3D11NvDecoder_Impl::HandlePictureDecode(void* userData, CUVIDPICPARAMS* pictureParams)
{
	return reinterpret_cast<D3D11NvDecoder_Impl*>(userData)->OnPictureDecode(pictureParams);
}

int32_t CUDAAPI D3D11NvDecoder_Impl::HandlePictureDisplay(void* userData, CUVIDPARSERDISPINFO* displayInfo)
{
	return reinterpret_cast<D3D11NvDecoder_Impl*>(userData)->OnPictureDisplay(displayInfo);
}

int32_t D3D11NvDecoder_Impl::OnVideoSequence(CUVIDEOFORMAT* videoFormat)
{
	// NVDEC Decoder 생성, 재설정
	// 스트림의 시작 또는 해상도, 포맷 변경 시 OnVideoSequence 가 호출된다.

	if (!videoFormat)
	{
		return 0;
	}

	const int32_t decodeSurfaceCount = videoFormat->min_num_decode_surfaces;

	CUVIDDECODECAPS decodeCaps = {};
	decodeCaps.eCodecType = videoFormat->codec;
	decodeCaps.eChromaFormat = videoFormat->chroma_format;
	decodeCaps.nBitDepthMinus8 = videoFormat->bit_depth_luma_minus8;

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return 0;
	}

	// GPU 가 이 코덱과 해상도가 지원하는지 확인
	if (!NVDEC_API_CALL(cuvidGetDecoderCaps(&decodeCaps)))
	{
		return 0;
	}

	if (!decodeCaps.bIsSupported)
	{
		return 0;
	}

	if ((videoFormat->coded_width > decodeCaps.nMaxWidth) ||
		(videoFormat->coded_height > decodeCaps.nMaxHeight))
	{
		return 0;
	}

	if ((videoFormat->coded_width >> 4) * (videoFormat->coded_height >> 4) > decodeCaps.nMaxMBCount)
	{
		return 0;
	}

	if (m_videoFormatDesc.isInitialized && m_decoder)
	{
		// 이전에 Decoder 가 생성 된 경우라면 이곳을 타게 될 것이다.
		if (m_videoFormatDesc.lumaWidth && m_videoFormatDesc.lumaHeight && m_videoFormatDesc.chromaHeight)
		{
			// 필요에 따라 해상도가 변경되거나 Dispaly Area 변경된 경우 Decoder 파라메터를 Reconfigure 한다.
			const int32_t result = ReconfigureDecoder(videoFormat);
			if (result == 0 || result == 1)
			{
				return result;
			}
			return decodeSurfaceCount;
		}
	}

	// 여기 아래서부터는 초기 1회에 대해서 Decoder 를 생성하는 부분이다.

	// NVDEC 출력 포맷 설정
	VideoFormatDesc videoFormatDesc = {};
	videoFormatDesc.isInitialized = true;
	videoFormatDesc.eCodec = videoFormat->codec;
	videoFormatDesc.eChromaFormat = videoFormat->chroma_format;
	videoFormatDesc.bitDepthMinus8 = videoFormat->bit_depth_luma_minus8;
	videoFormatDesc.bitPerPixel = videoFormat->bit_depth_luma_minus8 > 0 ? 2 : 1;

	if (videoFormat->chroma_format == cudaVideoChromaFormat_420 || videoFormat->chroma_format == cudaVideoChromaFormat_Monochrome)
		videoFormatDesc.eOutputFormat = videoFormat->bit_depth_luma_minus8 ? cudaVideoSurfaceFormat_P016 : cudaVideoSurfaceFormat_NV12;
	else if (videoFormat->chroma_format == cudaVideoChromaFormat_444)
		videoFormatDesc.eOutputFormat = videoFormat->bit_depth_luma_minus8 ? cudaVideoSurfaceFormat_YUV444_16Bit : cudaVideoSurfaceFormat_YUV444;
	else if (videoFormat->chroma_format == cudaVideoChromaFormat_422)
		videoFormatDesc.eOutputFormat = videoFormat->bit_depth_luma_minus8 ? cudaVideoSurfaceFormat_P216 : cudaVideoSurfaceFormat_NV16;

	videoFormatDesc.codedWidth = videoFormat->coded_width;
	videoFormatDesc.codedHeight = videoFormat->coded_height;
	videoFormatDesc.maxCodedWidth = videoFormat->coded_width;
	videoFormatDesc.maxCodedHeight = videoFormat->coded_height;
	videoFormatDesc.lumaWidth = videoFormat->display_area.right - videoFormat->display_area.left;
	videoFormatDesc.lumaHeight = videoFormat->display_area.bottom - videoFormat->display_area.top;
	videoFormatDesc.chromaHeight = static_cast<uint32_t>(ceil(videoFormatDesc.lumaHeight * GetChromaHeightFactor(videoFormatDesc.eOutputFormat)));
	videoFormatDesc.chromaPlanes = GetChromaPlaneCount(videoFormatDesc.eOutputFormat);
	videoFormatDesc.eInterlaceMode = videoFormat->progressive_sequence ? cudaVideoDeinterlaceMode_Weave : cudaVideoDeinterlaceMode_Adaptive;
	videoFormatDesc.decodeSurfaceCount = static_cast<uint32_t>(decodeSurfaceCount);

	m_videoFormatDesc = videoFormatDesc;
	m_cuVideoFormat = *videoFormat;

	// Decoder 생성을 위한 파라메터 설정
	CUVIDDECODECREATEINFO decodeCreateInfo = {};
	decodeCreateInfo.CodecType = videoFormatDesc.eCodec;
	decodeCreateInfo.ulWidth = videoFormatDesc.codedWidth;
	decodeCreateInfo.ulHeight = videoFormatDesc.codedHeight;
	decodeCreateInfo.ulMaxWidth = videoFormatDesc.maxCodedWidth;
	decodeCreateInfo.ulMaxHeight = videoFormatDesc.maxCodedHeight;
	decodeCreateInfo.ulNumDecodeSurfaces = videoFormatDesc.decodeSurfaceCount;
	decodeCreateInfo.ChromaFormat = videoFormatDesc.eChromaFormat;
	decodeCreateInfo.bitDepthMinus8 = videoFormatDesc.bitDepthMinus8;
	decodeCreateInfo.OutputFormat = videoFormatDesc.eOutputFormat;
	decodeCreateInfo.DeinterlaceMode = videoFormatDesc.eInterlaceMode;
	decodeCreateInfo.ulNumOutputSurfaces = 2;
	decodeCreateInfo.ulTargetWidth = videoFormatDesc.codedWidth;
	decodeCreateInfo.ulTargetHeight = videoFormatDesc.lumaHeight;
	decodeCreateInfo.vidLock = m_ctxLock;
	decodeCreateInfo.ulCreationFlags = cudaVideoCreate_PreferCUVID;

	// NVDEC Decoder 생성
	if (!NVDEC_API_CALL(cuvidCreateDecoder(&m_decoder, &decodeCreateInfo)))
	{
		m_videoFormatDesc = {};
		ZeroMemory(&m_cuVideoFormat, sizeof(m_cuVideoFormat));
		return 0;
	}

	// Decode 결과를 저장할 출력 버퍼 생성
	if (!CreateTexturePool() || !CreateCudaDeviceMemoryPool())
	{
		DestroyTexturePool();
		DestroyCudaDeviceMemoryPool();
		NVDEC_API_CALL(cuvidDestroyDecoder(m_decoder));
		m_decoder = nullptr;
		m_videoFormatDesc = {};
		::ZeroMemory(&m_cuVideoFormat, sizeof(m_cuVideoFormat));
		return 0;
	}

	m_writeIndex = 0;
	m_readIndex = 0;

	return decodeSurfaceCount;
}

int32_t D3D11NvDecoder_Impl::OnPictureDecode(CUVIDPICPARAMS* pictureParams)
{
	// 프레임 하나 Decode Request
	// 실제 Decode 수행을 요청

	if (!m_decoder || !pictureParams)
	{
		return 0;
	}

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return -1;
	}

	return NVDEC_API_CALL(cuvidDecodePicture(m_decoder, pictureParams)) ? 1 : -1;
}

int32_t D3D11NvDecoder_Impl::OnPictureDisplay(CUVIDPARSERDISPINFO* displayInfo)
{
	// Decode 결과를 가져와서 NV12 -> BGRA 변환 후 사전에 등록된 BGRA D3D11Texture2D 에 저장한다.
	// 이때 NV12 -> BGRA 변환 작업은 VideoProcessor 가 아닌 Cuda Kernel 을 통해 작동한다.
	if (!m_decoder || !displayInfo)
	{
		return 0;
	}

	// Reconfigure 가 수행 중이라면 버퍼 초기화가 수행되므로 함수를 그냥 빠져나가도록 한다.
	if (::InterlockedCompareExchange(&m_reconfiguring, FALSE, FALSE) == TRUE)
	{
		return 0;
	}

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return -1;
	}

	CUVIDPROCPARAMS vidProcParameters = {};
	vidProcParameters.progressive_frame = displayInfo->progressive_frame;
	vidProcParameters.top_field_first = displayInfo->top_field_first;
	vidProcParameters.second_field = displayInfo->repeat_first_field;
	vidProcParameters.output_stream = m_cuStream;

	CUdeviceptr srcFrame = 0;   // Decode 결과 NVDEC 내부 프레임 포인터가 저장 되어 있다
	unsigned int srcPitch = 0;
	int32_t result = 1;
	bool mappedVideoFrame = false;
	bool mappedGraphicsResource = false;
	int32_t index = -1;
	CUarray cuArrayTexture = nullptr;
	CUDA_MEMCPY2D copy = {};

	if (!NVDEC_API_CALL(cuvidCtxLock(m_ctxLock, 0)))
	{
		return -1;
	}

	// Decoding 된 프레임 가져오기
	if (!NVDEC_API_CALL(cuvidMapVideoFrame(m_decoder, displayInfo->picture_index, &srcFrame, &srcPitch, &vidProcParameters)))
	{
		NVDEC_API_CALL(cuvidCtxUnlock(m_ctxLock, 0));
		return -1;
	}
	mappedVideoFrame = true;

	// Atomic 하게 Current Write, Read Pos 를 읽어온다.
	LONG currentWrite = ::InterlockedCompareExchange(&m_writeIndex, 0, 0);
	LONG currentRead = ::InterlockedCompareExchange(&m_readIndex, 0, 0);
	if ((currentWrite - currentRead) >= TEXTURE_POOL_COUNT)
	{
		::InterlockedExchange(&m_readIndex, currentWrite - TEXTURE_POOL_COUNT + 1);
	}

	// 사전에 등록된 D3D11Texture2D 에 Map
	index = currentWrite & (TEXTURE_POOL_COUNT - 1);
	if (!CUDA_DRVAPI_CALL(cuGraphicsMapResources(1, &m_cudaResources[index], m_cuStream)))
	{
		result = -1;
		goto cleanup;
	}
	mappedGraphicsResource = true;

	// Decode 결과는 NV12(YUC 4:2:0) 이므로 Cuda Kernel 로 BGRA 로 변환한다.
	{
		uint8_t* yPlane = reinterpret_cast<uint8_t*>(srcFrame);
		uint8_t* uvPlane = yPlane + srcPitch * m_videoFormatDesc.lumaHeight;

		SaveNV12ToRawFile(srcFrame, srcPitch, L"../DecodeResult.yuv");

		ConvertNV12ToBGRA(
			yPlane,
			uvPlane,
			m_videoFormatDesc.lumaWidth,
			m_videoFormatDesc.lumaHeight,
			srcPitch,
			reinterpret_cast<uchar4*>(m_cuBGRABuffer[index]),
			static_cast<int32_t>(m_cuBGRAPitch),
			m_cuStream);
	}

	// NV12 -> BGRA8 변환이 끝난 후 D3D11 Texture 로 데이터 복사
	if (!CUDA_DRVAPI_CALL(cuGraphicsSubResourceGetMappedArray(&cuArrayTexture, m_cudaResources[index], 0, 0)))
	{
		result = -1;
		goto cleanup;
	}

	// Device to Device 로의 복사가 수행되어 매우 빠르다.
	copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
	copy.srcDevice = m_cuBGRABuffer[index];
	copy.srcPitch = m_cuBGRAPitch;
	copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
	copy.dstArray = cuArrayTexture;
	copy.WidthInBytes = m_videoFormatDesc.lumaWidth * 4;
	copy.Height = m_videoFormatDesc.lumaHeight;

	if (!CUDA_DRVAPI_CALL(cuMemcpy2DAsync(&copy, m_cuStream)))
	{
		result = -1;
		goto cleanup;
	}

	// 해당 인덱스에 Decode 가 완료 이벤트 설정
	if (!CUDA_DRVAPI_CALL(cuEventRecord(m_cuEvents[index], m_cuStream)))
	{
		result = -1;
		goto cleanup;
	}

	//if (!CUDA_DRVAPI_CALL(cuStreamSynchronize(m_cuStream))) // GPU 작업 완료 대기
	//{
	//    return -1;
	//}

	// Timestamp 를 기록하고 다음 버퍼에 Write 하기 위한 인덱스 증가
	m_frames[index].timestamp = displayInfo->timestamp;
	::MemoryBarrier();
	::InterlockedIncrement(&m_writeIndex);

	// 사용이 끝난 Resource Unmap 처리
cleanup:
	if (mappedGraphicsResource)
	{
		CUDA_DRVAPI_CALL(cuGraphicsUnmapResources(1, &m_cudaResources[index], m_cuStream));
	}

	if (mappedVideoFrame)
	{
		NVDEC_API_CALL(cuvidUnmapVideoFrame(m_decoder, srcFrame));
	}

	NVDEC_API_CALL(cuvidCtxUnlock(m_ctxLock, 0));
	return result;
}

bool D3D11NvDecoder_Impl::InitializeCuda()
{
	// 디코더 생성을 위한 Cuda Driver 초기화
	// Cuda Context 생성/획득
	// Cuda Context 생성/획득
	// NVDEC 사용을 위한 ctxLock, Stream, Event, Parser 리소스 생성


	// Cuda Driver API 사용 전에 cuInit 호출 필수
	if (!CUDA_DRVAPI_CALL(cuInit(0)))
	{
		return false;
	}

	bool deviceFound = false;
	IDXGIDevice* dxgiDevice = nullptr;
	IDXGIAdapter* adapter = nullptr;

	// D3D11 <-> CUDA Device 매칭
	// Multi-GPU 사용 시를 고려하여 Cuda 와 D3D11 이 동일한 GPU 를 사용하도록 맞춘다.
	if (SUCCEEDED(m_D3D11Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))))
	{
		if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
		{
			if (CUDA_DRVAPI_CALL(cuD3D11GetDevice(&m_cudaDevice, adapter)))
			{
				deviceFound = true;
			}
			adapter->Release();
		}
		dxgiDevice->Release();
	}

	// 찾지 못했으면 기본값으로 0번 GPU 사용
	if (!deviceFound && !CUDA_DRVAPI_CALL(cuDeviceGet(&m_cudaDevice, 0)))
	{
		return false;
	}

	// NVDEC 는 Primary Context 사용을 권장한다.
	// CreateCudaContext 는 Depricated 되었다.
	if (!CUDA_DRVAPI_CALL(cuDevicePrimaryCtxRetain(&m_cudaContext, m_cudaDevice)))
	{
		return false;
	}

	if (!CUDA_DRVAPI_CALL(cuCtxPushCurrent(m_cudaContext)))
	{
		CUDA_DRVAPI_CALL(cuDevicePrimaryCtxRelease(m_cudaDevice));
		m_cudaContext = nullptr;
		return false;
	}

	bool success = false;
	do
	{
		// NVDEC 내부 동기화 Lock 생성
		if (!NVDEC_API_CALL(cuvidCtxLockCreate(&m_ctxLock, m_cudaContext)))
		{
			break;
		}

		// 비동기 처리를 위한 Cuda Stream 생성
		if (!CUDA_DRVAPI_CALL(cuStreamCreate(&m_cuStream, CU_STREAM_NON_BLOCKING)))
		{
			break;
		}

		// Decode 완료 처리를 송수신 하기 위한 Cuda Event 생성
		// Cuda 명령어 사이에 Event 를 넣어서 정확한 시점에 이벤트 동기화를 수행
		for (int32_t i = 0; i < TEXTURE_POOL_COUNT; ++i)
		{
			if (!CUDA_DRVAPI_CALL(cuEventCreate(&m_cuEvents[i], CU_EVENT_DISABLE_TIMING)))
			{
				break;
			}
		}

		if (!m_cuEvents[TEXTURE_POOL_COUNT - 1])
		{
			break;
		}

		// NVDEC Parser 생성
		// cuvidParseVideoData 를 호출하면 내부 Callback 구조로
		// HandleVideoSequence -> HandlePictureDecode -> HandlePictureDisplay 가 호출된다.
		// 이 코드에서는 H.264 코덱 기준으로 디코더를 생성.
		CUVIDPARSERPARAMS parserParameters = {};
		parserParameters.CodecType = cudaVideoCodec_H264;
		parserParameters.ulMaxNumDecodeSurfaces = 20;
		parserParameters.ulMaxDisplayDelay = 1;
		parserParameters.pUserData = this;
		parserParameters.pfnSequenceCallback = HandleVideoSequence;
		parserParameters.pfnDecodePicture = HandlePictureDecode;
		parserParameters.pfnDisplayPicture = HandlePictureDisplay;

		if (!NVDEC_API_CALL(cuvidCreateVideoParser(&m_parser, &parserParameters)))
		{
			break;
		}

		success = true;
	} while (false);

	CUDA_DRVAPI_CALL(cuCtxPopCurrent(nullptr));

	if (!success)
	{
		ShutDown();
	}

	return success;
}

bool D3D11NvDecoder_Impl::ReconfigureDecoder(CUVIDEOFORMAT* videoFormat)
{
	// NVDEC 해상도가 변경되거나 Dispaly Area 가 변경된 경우
	// Decode 중인 Frame 의 진행 완료를 대기한 후에
	// 변경된 해상도, Display Area 에 맞추어
	// 텍스쳐풀, 메모리풀을 삭제 후 Reconfigure 후 텍스쳐풀, 메모리풀을 다시 생성한다.

	if (!m_decoder || !videoFormat)
	{
		return false;
	}

	ScopedReconfigureFlag reconfigureFlag(&m_reconfiguring);

	if (videoFormat->bit_depth_luma_minus8 != m_cuVideoFormat.bit_depth_luma_minus8 ||
		videoFormat->bit_depth_chroma_minus8 != m_cuVideoFormat.bit_depth_chroma_minus8)
	{
		return 0;
	}

	if (videoFormat->chroma_format != m_cuVideoFormat.chroma_format)
	{
		return 0;
	}

	const bool isDecodeResChange = !(videoFormat->coded_width == m_cuVideoFormat.coded_width && videoFormat->coded_height == m_cuVideoFormat.coded_height);
	const bool isDisplayRectChange =
		!(videoFormat->display_area.bottom == m_cuVideoFormat.display_area.bottom &&
			videoFormat->display_area.top == m_cuVideoFormat.display_area.top &&
			videoFormat->display_area.left == m_cuVideoFormat.display_area.left &&
			videoFormat->display_area.right == m_cuVideoFormat.display_area.right);

	if (!isDecodeResChange && isDisplayRectChange)
	{
		// Display Rectangle 만 변경된 경우
		// VideoFormatDesc 만 업데이트 하고 종료
		m_videoFormatDesc.lumaWidth = videoFormat->display_area.right - videoFormat->display_area.left;
		m_videoFormatDesc.lumaHeight = videoFormat->display_area.bottom - videoFormat->display_area.top;
		m_videoFormatDesc.chromaHeight = static_cast<uint32_t>(ceil(m_videoFormatDesc.lumaHeight * GetChromaHeightFactor(m_videoFormatDesc.eOutputFormat)));
		m_videoFormatDesc.chromaPlanes = GetChromaPlaneCount(m_videoFormatDesc.eOutputFormat);
		m_cuVideoFormat = *videoFormat;
		return 1;
	}

	if (!isDecodeResChange)
	{
		// Decoder 해상도가 변경되지 않은 경우라면
		// VideoFormat 만 업데이트 하고 종료
		m_cuVideoFormat = *videoFormat;
		return 1;
	}


	// 여기까지 온 경우라면 디코더 해상도가 변경된 경우
	// 리소스를 해제할 예정이므로  현재 디코딩 중 인 모든 프레임의 디코딩이 종료될 때 까지 대기
	WaitForAllFrames();

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return 0;
	}

	// 리소스 해제
	DestroyTexturePool();
	DestroyCudaDeviceMemoryPool();

	// VdieoFormatDesc 업데이트
	m_videoFormatDesc.codedWidth = videoFormat->coded_width;
	m_videoFormatDesc.codedHeight = videoFormat->coded_height;
	m_videoFormatDesc.lumaWidth = videoFormat->display_area.right - videoFormat->display_area.left;
	m_videoFormatDesc.lumaHeight = videoFormat->display_area.bottom - videoFormat->display_area.top;
	m_videoFormatDesc.chromaHeight = static_cast<uint32_t>(ceil(m_videoFormatDesc.lumaHeight * GetChromaHeightFactor(m_videoFormatDesc.eOutputFormat)));
	m_videoFormatDesc.chromaPlanes = GetChromaPlaneCount(m_videoFormatDesc.eOutputFormat);
	if (m_videoFormatDesc.decodeSurfaceCount < videoFormat->min_num_decode_surfaces)
	{
		m_videoFormatDesc.decodeSurfaceCount = videoFormat->min_num_decode_surfaces;
	}

	// Cuda NVDEC Reconfigure 옵션 설정 후 Reconfigure 수행
	CUVIDRECONFIGUREDECODERINFO reconfigureParameters = {};
	reconfigureParameters.ulWidth = m_videoFormatDesc.codedWidth;
	reconfigureParameters.ulHeight = m_videoFormatDesc.codedHeight;
	reconfigureParameters.display_area.left = videoFormat->display_area.left;
	reconfigureParameters.display_area.top = videoFormat->display_area.top;
	reconfigureParameters.display_area.right = videoFormat->display_area.right;
	reconfigureParameters.display_area.bottom = videoFormat->display_area.bottom;
	reconfigureParameters.ulTargetWidth = m_videoFormatDesc.codedWidth;
	reconfigureParameters.ulTargetHeight = m_videoFormatDesc.codedHeight;
	reconfigureParameters.ulNumDecodeSurfaces = m_videoFormatDesc.decodeSurfaceCount;

	if (!NVDEC_API_CALL(cuvidReconfigureDecoder(m_decoder, &reconfigureParameters)))
	{
		return 0;
	}

	// 새로운 해상도에 맞추어 리소스 재생성
	if (!CreateTexturePool() || !CreateCudaDeviceMemoryPool())
	{
		DestroyTexturePool();
		DestroyCudaDeviceMemoryPool();
		return 0;
	}

	// 기타 정보 재생성
	m_cuVideoFormat = *videoFormat;
	m_writeIndex = 0;
	m_readIndex = 0;

	return m_videoFormatDesc.decodeSurfaceCount;
}

bool D3D11NvDecoder_Impl::CreateTexturePool()
{
	// Cuda 가 Access 할 수 있는 D3D11 Resource 로 등록

	// 디코딩 완료 시점에(HandlePictureDisplay) Nv12 Surface 를 D3D11 가 사용하기 좋은
	// BGRA 32Bit 로 변환하여 D3D11 Texture2D 에 프레임을 저장한다.
	// 그리고 외부에서 디코딩이 완료되어 준비된 이 Texture 를 가져가 렌더링을 수행한다.

	// 이때, D3D11Texture2D 를 매번 생성/해제 하지 않고 미리 Pool 형태로 만들어 둔 후
	// CUDA 와 Interop 하여 사전에 RegisterResource 하는 방식으로 등록을 해두고
	// BGRA 32bit Frame 을 D3D11 Texture2D 에 저장하기 위한 텍스쳐 풀 생성

	const bool resizeRequired = (m_cacheTextureWidth != m_videoFormatDesc.lumaWidth) ||
		(m_cacheTextureHeight != m_videoFormatDesc.lumaHeight);

	if (!resizeRequired && m_frames[0].texture != nullptr)
	{
		return true;
	}

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return false;
	}

	DestroyTexturePool();
	m_cacheTextureWidth = m_videoFormatDesc.lumaWidth;
	m_cacheTextureHeight = m_videoFormatDesc.lumaHeight;

	// 실제 Display 해상도 만큼의 BGRA 텍스쳐 생성
	// BindFlags 옵션 주의
	for (int32_t i = 0; i < TEXTURE_POOL_COUNT; ++i)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = m_videoFormatDesc.lumaWidth;
		desc.Height = m_videoFormatDesc.lumaHeight;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		// 텍스쳐 생성
		HRESULT hr = m_D3D11Device->CreateTexture2D(&desc, nullptr, &m_textures[i]);
		if (FAILED(hr))
		{
			DestroyTexturePool();
			return false;
		}

		// Cuda 가 Access 할 수 있는 D3D11 Resource 로 등록 
		if (!CUDA_DRVAPI_CALL(cuGraphicsD3D11RegisterResource(
			&m_cudaResources[i],
			m_textures[i],
			CU_GRAPHICS_REGISTER_FLAGS_NONE)))
		{
			DestroyTexturePool();
			return false;
		}

		m_frames[i].texture = m_textures[i];
		m_frames[i].timestamp = 0;
	}

	return true;
}

void D3D11NvDecoder_Impl::DestroyTexturePool()
{
	// 리소스 생성 순서와 반대로 리소스 해제

	for (int32_t i = 0; i < TEXTURE_POOL_COUNT; ++i)
	{
		if (m_cudaResources[i])
		{
			CUDA_DRVAPI_CALL(cuGraphicsUnregisterResource(m_cudaResources[i]));
			m_cudaResources[i] = nullptr;
		}

		if (m_textures[i])
		{
			m_textures[i]->Release();
			m_textures[i] = nullptr;
		}

		m_frames[i].texture = nullptr;
		m_frames[i].timestamp = 0;
	}
}

bool D3D11NvDecoder_Impl::CreateCudaDeviceMemoryPool()
{
	// BGRA 32 Bit 변환 결과가 저장될 Cuda Memory 생성

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return false;
	}

	DestroyCudaDeviceMemoryPool();
	m_cuBGRAPitch = 0;

	for (int32_t i = 0; i < TEXTURE_POOL_COUNT; ++i)
	{
		// GPU 내부 메모리 구조에 맞추어 Aligned 된 메모리를 생성하기 위해
		// cuMemAllocPitch 사용.
		if (!CUDA_DRVAPI_CALL(cuMemAllocPitch(
			&m_cuBGRABuffer[i],
			&m_cuBGRAPitch,
			m_videoFormatDesc.lumaWidth * 4,
			m_videoFormatDesc.lumaHeight,
			16)))
		{
			DestroyCudaDeviceMemoryPool();
			return false;
		}
	}

	return true;
}

void D3D11NvDecoder_Impl::DestroyCudaDeviceMemoryPool()
{
	// Cuda Device 메모리 해제
	for (int32_t i = 0; i < TEXTURE_POOL_COUNT; ++i)
	{
		if (m_cuBGRABuffer[i])
		{
			CUDA_DRVAPI_CALL(cuMemFree(m_cuBGRABuffer[i]));
			m_cuBGRABuffer[i] = 0;
		}
	}
	m_cuBGRAPitch = 0;
}

void D3D11NvDecoder_Impl::WaitForAllFrames()
{
	// cuEvent 로 모든 프레임이 Idle 상태인지 체크
	// Decode 중 이라면 cuEventSynchronize 로 대기
	// HandlePictureDisplay 호출 종료 시점에 Event Set.

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return;
	}

	for (int32_t i = 0; i < TEXTURE_POOL_COUNT; ++i)
	{
		if (!m_cuEvents[i])
		{
			continue;
		}

		if (cuEventQuery(m_cuEvents[i]) == CUDA_SUCCESS)
		{
			continue;
		}

		CUDA_DRVAPI_CALL(cuEventSynchronize(m_cuEvents[i]));
	}
}

bool D3D11NvDecoder_Impl::SaveFrameToBmp(int32_t index, const wchar_t* fileName)
{
	if (index < 0 || index >= TEXTURE_POOL_COUNT || !m_textures[index])
		return false;

	ID3D11Texture2D* gpuTexture = m_textures[index];
	D3D11_TEXTURE2D_DESC desc = {};
	gpuTexture->GetDesc(&desc);

	// 1. CPU에서 읽기 위한 Staging Texture 생성
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;

	ID3D11Texture2D* stagingTexture = nullptr;
	if (FAILED(m_D3D11Device->CreateTexture2D(&desc, nullptr, &stagingTexture)))
		return false;

	// 2. GPU -> Staging으로 데이터 복사
	m_D3D11Context->CopyResource(stagingTexture, gpuTexture);

	// 3. 데이터 Map
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(m_D3D11Context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		stagingTexture->Release();
		return false;
	}

	FILE* fp = nullptr;
	if (_wfopen_s(&fp, fileName, L"wb") != 0 || fp == nullptr)
	{
		m_D3D11Context->Unmap(stagingTexture, 0);
		SafeRelease(stagingTexture);
		return false;
	}

	// 4. BMP 파일 생성 (Header 작성)
	BITMAPFILEHEADER bfh = {};
	bfh.bfType = 0x4D42; // "BM"
	bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
	bfh.bfSize = bfh.bfOffBits + (desc.Width * desc.Height * 4);

	BITMAPINFOHEADER bih = {};
	bih.biSize = sizeof(BITMAPINFOHEADER);
	bih.biWidth = desc.Width;
	bih.biHeight = -(long)desc.Height; // Top-down
	bih.biPlanes = 1;
	bih.biBitCount = 32;
	bih.biCompression = BI_RGB;

	// Header 쓰기
	fwrite(&bfh, sizeof(bfh), 1, fp);
	fwrite(&bih, sizeof(bih), 1, fp);

	// Row by row copy (Pitch 고려)
	uint8_t* pSource = reinterpret_cast<uint8_t*>(mapped.pData);
	for (uint32_t y = 0; y < desc.Height; ++y)
	{
		// 4바이트(RGBA) * 너비만큼 쓰기
		fwrite(pSource + (y * mapped.RowPitch), desc.Width * 4, 1, fp);
	}

	fclose(fp);
	m_D3D11Context->Unmap(stagingTexture, 0);
	stagingTexture->Release();

	return true;
}


bool D3D11NvDecoder_Impl::SaveNV12ToRawFile(CUdeviceptr srcFrame, unsigned int srcPitch, const wchar_t* fileName)
{
	uint32_t width = m_videoFormatDesc.lumaWidth;
	uint32_t height = m_videoFormatDesc.lumaHeight;

	// 1. CPU 임시 버퍼 할당 (Y: width*height, UV: width*height/2)
	size_t size = width * height * 3 / 2;
	uint8_t* cpuBuffer = new uint8_t[size];

	if (!cpuBuffer)
		return false;

	// 2. Y Plane 복사 (Pitch 고려)
	CUDA_MEMCPY2D copyY = {};
	copyY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
	copyY.srcDevice = srcFrame;
	copyY.srcPitch = srcPitch;
	copyY.dstMemoryType = CU_MEMORYTYPE_HOST;
	copyY.dstHost = cpuBuffer;
	copyY.dstPitch = width;
	copyY.WidthInBytes = width;
	copyY.Height = height;

	if (!CUDA_DRVAPI_CALL(cuMemcpy2D(&copyY)))
	{
		if (cpuBuffer)
		{
			delete[]cpuBuffer;
			cpuBuffer = nullptr;
		}
		return false;
	}

	// 3. UV Plane 복사
	CUDA_MEMCPY2D copyUV = {};
	copyUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
	copyUV.srcDevice = srcFrame + (srcPitch * height);
	copyUV.srcPitch = srcPitch;
	copyUV.dstMemoryType = CU_MEMORYTYPE_HOST;
	copyUV.dstHost = cpuBuffer + (width * height);
	copyUV.dstPitch = width;
	copyUV.WidthInBytes = width;
	copyUV.Height = height / 2;

	if (!CUDA_DRVAPI_CALL(cuMemcpy2D(&copyUV)))
	{
		if (cpuBuffer)
		{
			delete[]cpuBuffer;
			cpuBuffer = nullptr;
		}
		return false;
	}

	// 4. 파일 쓰기
	FILE* fp = nullptr;
	if (_wfopen_s(&fp, fileName, L"wb") != 0 || fp == nullptr)
	{
		if (cpuBuffer)
		{
			delete[]cpuBuffer;
			cpuBuffer = nullptr;
		}
		return false;
	}

	size_t written = fwrite(cpuBuffer, 1, size, fp);
	fclose(fp);

	if (cpuBuffer)
	{
		delete[]cpuBuffer;
		cpuBuffer = nullptr;
	}

	return (written == size);
}
