#include "pch.h"
#include "D3D11NvDecoder_Impl.h"

#include "DecodeThread.h"

#include <cudaD3D11.h>
#include <malloc.h>
#include <stdio.h> // for printf_s, fopen_s, fwrite
#include <math.h>


#include "../../D3D11EngineInterface/ID3D11ImmediateContextGate.h"
#include "ColorSpaceCuda.cuh"


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

	inline bool IsPowerOfTwo(uint32_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

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

D3D11NvDecoder_Impl::~D3D11NvDecoder_Impl()
{
	Destroy();
}

bool D3D11NvDecoder_Impl::Initialize(
	ID3D11Device* device,
	const NvDecConfig& config,
	ID3D11ImmediateContextGate* contextGate)
{
	if (!device)
	{
		return false;
	}

	// 슬롯이 1 개면 앱이 프레임을 들고 있는 동안 쓸 슬롯이 없어
	// 모든 프레임이 버려진다. 엔코더와 같은 이유로 최소 2 를 요구한다.
	if (config.outputSlotCount < kMinOutputSlotCount
		|| config.outputSlotCount > kMaxOutputSlotCount
		|| !IsPowerOfTwo(config.outputSlotCount))
	{
		printf_s("[NVDEC ERROR] Invalid outputSlotCount %u."
			" It must be a power of two between %u and %u.\n",
			config.outputSlotCount, kMinOutputSlotCount, kMaxOutputSlotCount);
		return false;
	}

	if (config.maxDecodeSurfaces == 0)
	{
		printf_s("[NVDEC ERROR] maxDecodeSurfaces must be non-zero.\n");
		return false;
	}

	Destroy();

	// 건네 받은 D3D11 Device, Context 포인터의 참조 횟수 증가
	// Destroy 시점에 Release 호출 필요
	m_D3D11Device = device;
	m_D3D11Device->AddRef();
	m_D3D11Device->GetImmediateContext(&m_D3D11Context);
	m_contextGate = contextGate;

	m_config = config;
	m_outputSlotCount = config.outputSlotCount;

	::InterlockedExchange(&m_writeSequence, 0);
	::InterlockedExchange(&m_readSequence, 0);
	::InterlockedExchange(&m_faulted, FALSE);
	::InterlockedExchange(&m_consecutiveLostFrames, 0);
	::InterlockedExchange(&m_framesHeldByApp, 0);
	::InterlockedExchange64(&m_parsedPacketCount, 0);
	::InterlockedExchange64(&m_decodedFrameCount, 0);
	::InterlockedExchange64(&m_deliveredFrameCount, 0);
	::InterlockedExchange64(&m_droppedPoolExhaustedCount, 0);
	::InterlockedExchange64(&m_droppedNotConsumedCount, 0);
	::InterlockedExchange64(&m_droppedDisplayFailedCount, 0);

	for (uint32_t slot = 0; slot < kMaxOutputSlotCount; ++slot)
	{
		::InterlockedExchange(&m_slotHeldByApp[slot], FALSE);
	}

	// 디코더 생성을 위한 Cuda Driver 초기화, Context 생성/획득,
	// NVDEC 사용을 위한 ctxLock / Stream / Event / Parser 리소스 생성
	if (!InitializeCuda())
	{
		Destroy();
		return false;
	}

	return true;
}

void D3D11NvDecoder_Impl::Destroy()
{
	// 큐 펌프를 먼저 멈춘다. 이게 살아 있으면 해체 중인 파서로 패킷이 계속 들어온다.
	StopDecodeThread();

	// 파서를 먼저 없애야 이후 콜백이 들어오지 않는다.
	if (m_parser)
	{
		NVDEC_API_CALL(cuvidDestroyVideoParser(m_parser));
		m_parser = nullptr;
	}

	const LONG heldByApp = ::ReadAcquire(&m_framesHeldByApp);
	if (heldByApp > 0)
	{
		// 앱이 아직 들고 있는 텍스처를 여기서 해제한다.
		// 앱이 쥔 포인터가 무효해지므로 조용히 넘어가지 않고 알린다.
		printf_s("[NVDEC WARNING] Destroying while the app still holds %ld frame(s)."
			" Their textures become invalid.\n", heldByApp);
	}

	if (m_cudaContext)
	{
		ScopedCudaContext cudaContext(m_cudaContext);
		if (cudaContext.IsActive())
		{
			// Decode 수행 중인 프레임이 있다면 전부 기다린 후에 해제한다.
			WaitForAllSlots();
			DestroyOutputSlots();
			DestroyBgraStagingBuffers();

			if (m_decoder)
			{
				NVDEC_API_CALL(cuvidDestroyDecoder(m_decoder));
				m_decoder = nullptr;
			}

			for (uint32_t slot = 0; slot < kMaxOutputSlotCount; ++slot)
			{
				if (m_decodeCompleteEvents[slot])
				{
					CUDA_DRVAPI_CALL(cuEventDestroy(m_decodeCompleteEvents[slot]));
					m_decodeCompleteEvents[slot] = nullptr;
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

	SetErrorCallback(nullptr, nullptr);

	m_videoFormatDesc = {};
	::ZeroMemory(&m_cuVideoFormat, sizeof(m_cuVideoFormat));
	::InterlockedExchange(&m_writeSequence, 0);
	::InterlockedExchange(&m_readSequence, 0);
	::InterlockedExchange(&m_reconfiguring, FALSE);
	::InterlockedExchange(&m_framesHeldByApp, 0);
	m_cachedTextureWidth = 0;
	m_cachedTextureHeight = 0;
	m_bgraStagingPitch = 0;
	m_outputSlotCount = 0;

	for (uint32_t slot = 0; slot < kMaxOutputSlotCount; ++slot)
	{
		::InterlockedExchange(&m_slotHeldByApp[slot], FALSE);
	}

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

	m_contextGate = nullptr;
}

bool D3D11NvDecoder_Impl::Parse(const uint8_t* data, uint32_t size, uint64_t timestamp,
	bool endOfPicture, bool endOfStream, bool discontinuity)
{
	// Decode Thread 가 호출하는 Decode Request 함수.
	// 이후 HandleVideoSequence -> HandlePictureDecode -> HandlePictureDisplay
	// 순서로 콜백이 호출된다.
	if (!m_parser || (!data && size > 0))
	{
		return false;
	}

	if (IsFaulted())
	{
		return false;
	}

	CUVIDSOURCEDATAPACKET packet = {};
	packet.payload = data;
	packet.payload_size = size;
	packet.timestamp = static_cast<CUvideotimestamp>(timestamp);
	packet.flags = CUVID_PKT_TIMESTAMP;

	if (endOfPicture)
		packet.flags |= CUVID_PKT_ENDOFPICTURE;
	if (endOfStream)
		packet.flags |= CUVID_PKT_ENDOFSTREAM;
	if (discontinuity)
		packet.flags |= CUVID_PKT_DISCONTINUITY;

	if (!NVDEC_API_CALL(cuvidParseVideoData(m_parser, &packet)))
	{
		// 비트스트림이 깨졌을 수 있다. 파이프라인은 유지하고 앱에 알린다.
		NoteLostFrame(NvDecErrorCode::ParseFailed);
		return false;
	}

	::InterlockedIncrement64(&m_parsedPacketCount);
	return true;
}

uint32_t D3D11NvDecoder_Impl::GetWriteSlotIndex() const
{
	const LONG sequence = ::ReadAcquire(&m_writeSequence);
	return static_cast<uint32_t>(sequence) & (m_outputSlotCount - 1U);
}

uint32_t D3D11NvDecoder_Impl::GetReadSlotIndex() const
{
	const LONG sequence = ::ReadAcquire(&m_readSequence);
	return static_cast<uint32_t>(sequence) & (m_outputSlotCount - 1U);
}

bool D3D11NvDecoder_Impl::IsSlotHeldByApp(uint32_t slot) const
{
	if (slot >= kMaxOutputSlotCount)
	{
		return false;
	}

	return ::ReadAcquire(&m_slotHeldByApp[slot]) == TRUE;
}

D3D11NvDecoder_Impl::Frame* D3D11NvDecoder_Impl::AcquireFrame()
{
	// 준비된 프레임을 하나 꺼낸다.
	// 돌려준 슬롯은 ReleaseFrame 이 불릴 때까지 디코더가 덮어쓰지 않는다.
	if (m_outputSlotCount == 0)
	{
		return nullptr;
	}

	LONG currentWrite = ::ReadAcquire(&m_writeSequence);
	LONG currentRead = ::ReadAcquire(&m_readSequence);

	if (currentRead >= currentWrite)
	{
		return nullptr;
	}

	// 앱이 못 따라가 밀렸으면 최신 쪽으로 건너뛴다.
	// 라이브 영상에서는 오래된 프레임을 전달하는 것보다 버리는 편이 낫다.
	const LONG lag = currentWrite - currentRead;
	const LONG maxLag = static_cast<LONG>(m_config.maxOutputLagFrames);
	if (maxLag > 0 && lag > maxLag)
	{
		const LONG skipTarget = currentWrite - 1;
		const LONG skipped = skipTarget - currentRead;
		if (skipped > 0)
		{
			::InterlockedExchange(&m_readSequence, skipTarget);
			::InterlockedExchangeAdd64(&m_droppedNotConsumedCount, skipped);
			currentRead = skipTarget;
			InvokeErrorCallback(NvDecErrorCode::OutputNotConsumed);
		}
	}

	const uint32_t slot = static_cast<uint32_t>(currentRead) & (m_outputSlotCount - 1U);

	// 이 슬롯을 앱이 이미 들고 있으면 안 된다. 링 장부가 어긋났다는 뜻이다.
	if (IsSlotHeldByApp(slot))
	{
		printf_s("[NVDEC ERROR] Output slot %u is still held by the app while being"
			" handed out again. Ring bookkeeping is inconsistent.\n", slot);
		return nullptr;
	}

	// Decode 완료 이벤트를 기다린 후에 넘긴다.
	if (m_decodeCompleteEvents[slot])
	{
		ScopedCudaContext cudaContext(m_cudaContext);
		if (!cudaContext.IsActive())
		{
			return nullptr;
		}

		if (!CUDA_DRVAPI_CALL(cuEventSynchronize(m_decodeCompleteEvents[slot])))
		{
			return nullptr;
		}
	}

	::InterlockedExchange(&m_slotHeldByApp[slot], TRUE);
	::InterlockedIncrement(&m_framesHeldByApp);
	::InterlockedIncrement(&m_readSequence);
	::InterlockedIncrement64(&m_deliveredFrameCount);

	m_frames[slot].slot = slot;
	return &m_frames[slot];
}

void D3D11NvDecoder_Impl::ReleaseFrame(Frame* frame)
{
	if (!frame)
	{
		return;
	}

	const uint32_t slot = frame->slot;
	if (slot >= m_outputSlotCount || frame != &m_frames[slot])
	{
		printf_s("[NVDEC ERROR] ReleaseFrame got a frame this decoder did not hand out.\n");
		return;
	}

	if (::InterlockedExchange(&m_slotHeldByApp[slot], FALSE) != TRUE)
	{
		printf_s("[NVDEC WARNING] ReleaseFrame called twice for slot %u.\n", slot);
		return;
	}

	::InterlockedDecrement(&m_framesHeldByApp);
}

bool D3D11NvDecoder_Impl::StartDecodeThread(DecodeFrameQueue* queue)
{
	if (!queue)
	{
		return false;
	}

	StopDecodeThread();

	m_decodeThread = new (std::nothrow) DecodeThread();
	if (!m_decodeThread)
	{
		return false;
	}

	m_decodeThread->SetFrameCallback(m_pendingFrameCallback, m_pendingFrameCallbackUserData);

	if (!m_decodeThread->Initialize(queue, this))
	{
		delete m_decodeThread;
		m_decodeThread = nullptr;
		return false;
	}

	return true;
}

void D3D11NvDecoder_Impl::StopDecodeThread()
{
	if (!m_decodeThread)
	{
		return;
	}

	m_decodeThread->Shutdown();
	delete m_decodeThread;
	m_decodeThread = nullptr;
}

void D3D11NvDecoder_Impl::SetFrameCallback(D3D11NvDecoder::FrameCallback callback, void* userData)
{
	// 스레드가 아직 없으면 기억해 뒀다가 StartDecodeThread 에서 넘긴다.
	m_pendingFrameCallback = callback;
	m_pendingFrameCallbackUserData = userData;

	if (m_decodeThread)
	{
		m_decodeThread->SetFrameCallback(callback, userData);
	}
}

void D3D11NvDecoder_Impl::SetErrorCallback(ErrorCallback callback, void* userData)
{
	::AcquireSRWLockExclusive(&m_callbackLock);
	m_errorCallback = callback;
	m_errorCallbackUserData = userData;
	::ReleaseSRWLockExclusive(&m_callbackLock);
}

void D3D11NvDecoder_Impl::InvokeErrorCallback(NvDecErrorCode errorCode)
{
	::AcquireSRWLockShared(&m_callbackLock);
	if (m_errorCallback)
	{
		m_errorCallback(errorCode, m_errorCallbackUserData);
	}
	::ReleaseSRWLockShared(&m_callbackLock);
}

void D3D11NvDecoder_Impl::NoteLostFrame(NvDecErrorCode errorCode)
{
	// 프레임 하나를 잃었다. 연속으로 쌓이면 세션이 살아있다고 볼 수 없다.
	InvokeErrorCallback(errorCode);

	const LONG consecutive = ::InterlockedIncrement(&m_consecutiveLostFrames);
	if (static_cast<uint32_t>(consecutive) >= m_config.maxConsecutiveLostFrames)
	{
		printf_s("[NVDEC ERROR] %ld consecutive frames lost. Giving up the session.\n",
			consecutive);
		EnterFaultedState(NvDecErrorCode::DecoderFaulted);
	}
}

void D3D11NvDecoder_Impl::NoteHealthyFrame()
{
	::InterlockedExchange(&m_consecutiveLostFrames, 0);
}

void D3D11NvDecoder_Impl::EnterFaultedState(NvDecErrorCode errorCode)
{
	const bool alreadyFaulted = (::InterlockedExchange(&m_faulted, TRUE) == TRUE);
	if (alreadyFaulted)
	{
		return;
	}

	printf_s("[NVDEC ERROR] Decoder entered faulted state. errorCode=%u\n",
		static_cast<uint32_t>(errorCode));
	InvokeErrorCallback(errorCode);
}

bool D3D11NvDecoder_Impl::IsFaulted() const
{
	return ::ReadAcquire(&m_faulted) == TRUE;
}

void D3D11NvDecoder_Impl::GetStats(NvDecStats& stats) const
{
	stats.parsedPackets = static_cast<uint64_t>(
		::ReadAcquire64(&m_parsedPacketCount));
	stats.decodedFrames = static_cast<uint64_t>(
		::ReadAcquire64(&m_decodedFrameCount));
	stats.deliveredFrames = static_cast<uint64_t>(
		::ReadAcquire64(&m_deliveredFrameCount));
	stats.droppedPoolExhausted = static_cast<uint64_t>(
		::ReadAcquire64(&m_droppedPoolExhaustedCount));
	stats.droppedNotConsumed = static_cast<uint64_t>(
		::ReadAcquire64(&m_droppedNotConsumedCount));
	stats.droppedDisplayFailed = static_cast<uint64_t>(
		::ReadAcquire64(&m_droppedDisplayFailedCount));
	stats.framesHeldByApp = static_cast<uint32_t>(
		::ReadAcquire(&m_framesHeldByApp));
	stats.faulted = IsFaulted();

	// 큐 펌프를 쓰지 않으면 packetsFailed 는 0 으로 남는다.
	if (m_decodeThread)
	{
		m_decodeThread->FillStats(stats);
	}
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
			// 필요에 따라 해상도가 변경되거나 Display Area 변경된 경우 Decoder 파라메터를 Reconfigure 한다.
			//
			// ReconfigureDecoder 는 파서에 돌려줄 값을 그대로 반환한다.
			// 예전에는 반환 타입이 bool 이라 decode surface 수가 1 로 뭉개졌고,
			// 그 아래 return decodeSurfaceCount 는 도달할 수 없는 코드였다.
			const int32_t reconfigureResult = ReconfigureDecoder(videoFormat);
			if (reconfigureResult == static_cast<int32_t>(SequenceResult::Failed))
			{
				EnterFaultedState(NvDecErrorCode::ReconfigureFailed);
			}

			return reconfigureResult;
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
	if (!CreateOutputSlots() || !CreateBgraStagingBuffers())
	{
		DestroyOutputSlots();
		DestroyBgraStagingBuffers();
		NVDEC_API_CALL(cuvidDestroyDecoder(m_decoder));
		m_decoder = nullptr;
		m_videoFormatDesc = {};
		::ZeroMemory(&m_cuVideoFormat, sizeof(m_cuVideoFormat));
		return 0;
	}

	m_writeSequence = 0;
	m_readSequence = 0;

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
	if (::ReadAcquire(&m_reconfiguring) == TRUE)
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
	uint32_t slot = 0;
	LONG currentWrite = 0;
	LONG currentRead = 0;
	CUarray cuArrayTexture = nullptr;
	CUDA_MEMCPY2D copy = {};

	// goto 로 건너뛰는 구간에 생성자가 있는 지역 객체를 둘 수 없다.
	// 이 함수가 이미 쓰는 mapped* 플래그와 같은 방식으로 게이트도 직접 여닫는다.
	bool gateEntered = false;

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
	currentWrite = ::ReadAcquire(&m_writeSequence);
	currentRead = ::ReadAcquire(&m_readSequence);
	if ((currentWrite - currentRead) >= static_cast<LONG>(m_outputSlotCount))
	{
		// 앱이 가져가지 않아 링이 한 바퀴 찼다. 오래된 것을 버리고 진행한다.
		const LONG skipTo = currentWrite - static_cast<LONG>(m_outputSlotCount) + 1;
		::InterlockedExchangeAdd64(&m_droppedNotConsumedCount, skipTo - currentRead);
		::InterlockedExchange(&m_readSequence, skipTo);
	}

	slot = static_cast<uint32_t>(currentWrite) & (m_outputSlotCount - 1U);

	// 앱이 아직 이 슬롯을 들고 있으면 덮어쓸 수 없다.
	// 여기서 덮어쓰면 앱이 렌더링 중인 텍스처가 찢어진다.
	// 이번 프레임을 버리고 앱에 알린다.
	if (IsSlotHeldByApp(slot))
	{
		::InterlockedIncrement64(&m_droppedPoolExhaustedCount);
		NVDEC_API_CALL(cuvidUnmapVideoFrame(m_decoder, srcFrame));
		NVDEC_API_CALL(cuvidCtxUnlock(m_ctxLock, 0));
		NoteLostFrame(NvDecErrorCode::OutputPoolExhausted);
		return 1;
	}

	// 여기부터 cleanup 까지 D3D11 리소스를 CUDA 에 매핑한 상태다.
	// 커널 실행과 memcpy 는 스트림에 실리는 비동기 명령이라 게이트를 오래 잡지 않는다.
	gateEntered = (m_contextGate != nullptr) && m_contextGate->Enter();

	// 사전에 등록된 D3D11Texture2D 에 Map
	if (!CUDA_DRVAPI_CALL(cuGraphicsMapResources(1, &m_cudaResources[slot], m_cuStream)))
	{
		result = -1;
		goto cleanup;
	}
	mappedGraphicsResource = true;

	// Decode 결과는 NV12(YUC 4:2:0) 이므로 Cuda Kernel 로 BGRA 로 변환한다.
	{
		uint8_t* yPlane = reinterpret_cast<uint8_t*>(srcFrame);
		uint8_t* uvPlane = yPlane + srcPitch * m_videoFormatDesc.lumaHeight;


		ConvertNV12ToBGRA(
			yPlane,
			uvPlane,
			m_videoFormatDesc.lumaWidth,
			m_videoFormatDesc.lumaHeight,
			srcPitch,
			reinterpret_cast<uchar4*>(m_bgraStagingBuffers[slot]),
			static_cast<int32_t>(m_bgraStagingPitch),
			m_cuStream);
	}

	// NV12 -> BGRA8 변환이 끝난 후 D3D11 Texture 로 데이터 복사
	if (!CUDA_DRVAPI_CALL(cuGraphicsSubResourceGetMappedArray(&cuArrayTexture, m_cudaResources[slot], 0, 0)))
	{
		result = -1;
		goto cleanup;
	}

	// Device to Device 로의 복사가 수행되어 매우 빠르다.
	copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
	copy.srcDevice = m_bgraStagingBuffers[slot];
	copy.srcPitch = m_bgraStagingPitch;
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
	if (!CUDA_DRVAPI_CALL(cuEventRecord(m_decodeCompleteEvents[slot], m_cuStream)))
	{
		result = -1;
		goto cleanup;
	}

	// 여기서 스트림을 동기화하지 않는다. 그러면 디코드 스레드가 매 프레임
	// GPU 변환·복사가 끝날 때까지 놀게 된다. 대기는 프레임이 실제로 필요한
	// AcquireFrame 으로 미뤄 두었고, 그 지점이 m_decodeCompleteEvents 를 기다린다.
	// 이것이 이 디코더의 async 구조다 — 엔코더의 완료 이벤트와 같은 역할이다.

	// 페이로드를 먼저 쓰고 시퀀스를 올린다. InterlockedIncrement 가 full barrier 라
	// 이 순서는 컴파일러도 CPU 도 뒤집지 못한다 — 소비자가 시퀀스를 본 시점에
	// timestamp 는 반드시 보인다. 예전에 있던 MemoryBarrier 는 중복이었다.
	//
	// 텍스처 내용은 이 배리어의 관할이 아니다. 그쪽은 GPU 가 쓰므로
	// m_decodeCompleteEvents 와 AcquireFrame 의 cuEventSynchronize 가 담당한다.
	m_frames[slot].timestamp = displayInfo->timestamp;
	::InterlockedIncrement(&m_writeSequence);
	::InterlockedIncrement64(&m_decodedFrameCount);
	NoteHealthyFrame();

	// 사용이 끝난 Resource Unmap 처리
cleanup:
	if (mappedGraphicsResource)
	{
		CUDA_DRVAPI_CALL(cuGraphicsUnmapResources(1, &m_cudaResources[slot], m_cuStream));
	}

	if (mappedVideoFrame)
	{
		NVDEC_API_CALL(cuvidUnmapVideoFrame(m_decoder, srcFrame));
	}

	NVDEC_API_CALL(cuvidCtxUnlock(m_ctxLock, 0));

	if (gateEntered)
	{
		m_contextGate->Leave();
	}

	return result;
}

bool D3D11NvDecoder_Impl::InitializeCuda()
{
	// 디코더 생성을 위한 Cuda Driver 초기화, Cuda Context 생성/획득,
	// NVDEC 사용을 위한 ctxLock, Stream, Event, Parser 리소스 생성까지 한다.

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
	// CreateCudaContext 는 Deprecated 되었다.
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
		for (uint32_t slot = 0; slot < m_outputSlotCount; ++slot)
		{
			if (!CUDA_DRVAPI_CALL(cuEventCreate(&m_decodeCompleteEvents[slot], CU_EVENT_DISABLE_TIMING)))
			{
				break;
			}
		}

		if (!m_decodeCompleteEvents[m_outputSlotCount - 1])
		{
			break;
		}

		// NVDEC Parser 생성
		// cuvidParseVideoData 를 호출하면 내부 Callback 구조로
		// HandleVideoSequence -> HandlePictureDecode -> HandlePictureDisplay 가 호출된다.
		// 이 코드에서는 H.264 코덱 기준으로 디코더를 생성.
		//
		// ulMaxDisplayDelay 0 은 저지연용이다. 파서가 표시 순서를 맞추려고
		// 프레임을 붙들지 않고 디코딩되는 즉시 내보낸다. B 프레임을 쓰면
		// 표시 순서가 어긋나지만, 이 파이프라인은 B 프레임을 쓰지 않는다.
		CUVIDPARSERPARAMS parserParameters = {};
		parserParameters.CodecType = cudaVideoCodec_H264;
		parserParameters.ulMaxNumDecodeSurfaces = m_config.maxDecodeSurfaces;
		parserParameters.ulMaxDisplayDelay = 0;
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
		Destroy();
	}

	return success;
}

int32_t D3D11NvDecoder_Impl::ReconfigureDecoder(CUVIDEOFORMAT* videoFormat)
{
	// NVDEC 해상도가 변경되거나 Display Area 가 변경된 경우
	// Decode 중인 Frame 의 진행 완료를 대기한 후에
	// 변경된 해상도, Display Area 에 맞추어
	// 텍스쳐풀, 메모리풀을 삭제 후 Reconfigure 후 텍스쳐풀, 메모리풀을 다시 생성한다.

	if (!m_decoder || !videoFormat)
	{
		return static_cast<int32_t>(SequenceResult::Failed);
	}

	ScopedReconfigureFlag reconfigureFlag(&m_reconfiguring);

	if (videoFormat->bit_depth_luma_minus8 != m_cuVideoFormat.bit_depth_luma_minus8 ||
		videoFormat->bit_depth_chroma_minus8 != m_cuVideoFormat.bit_depth_chroma_minus8)
	{
		return static_cast<int32_t>(SequenceResult::Failed);
	}

	if (videoFormat->chroma_format != m_cuVideoFormat.chroma_format)
	{
		return static_cast<int32_t>(SequenceResult::Failed);
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
		return static_cast<int32_t>(SequenceResult::KeepSurfaceCount);
	}

	if (!isDecodeResChange)
	{
		// Decoder 해상도가 변경되지 않은 경우라면
		// VideoFormat 만 업데이트 하고 종료
		m_cuVideoFormat = *videoFormat;
		return static_cast<int32_t>(SequenceResult::KeepSurfaceCount);
	}


	// 여기까지 온 경우라면 디코더 해상도가 변경된 경우
	// 리소스를 해제할 예정이므로  현재 디코딩 중 인 모든 프레임의 디코딩이 종료될 때 까지 대기
	WaitForAllSlots();

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return static_cast<int32_t>(SequenceResult::Failed);
	}

	// 리소스 해제
	DestroyOutputSlots();
	DestroyBgraStagingBuffers();

	// VideoFormatDesc 업데이트
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
		return static_cast<int32_t>(SequenceResult::Failed);
	}

	// 새로운 해상도에 맞추어 리소스 재생성
	if (!CreateOutputSlots() || !CreateBgraStagingBuffers())
	{
		DestroyOutputSlots();
		DestroyBgraStagingBuffers();
		return static_cast<int32_t>(SequenceResult::Failed);
	}

	// 기타 정보 재생성
	m_cuVideoFormat = *videoFormat;
	m_writeSequence = 0;
	m_readSequence = 0;

	return m_videoFormatDesc.decodeSurfaceCount;
}

bool D3D11NvDecoder_Impl::CreateOutputSlots()
{
	// 디코딩 완료 시점(OnPictureDisplay)에 NV12 서페이스를 BGRA 32bit 로 변환해
	// D3D11 Texture2D 에 저장한다. 앱은 그 텍스처를 그대로 렌더링에 쓴다.
	//
	// 텍스처를 매 프레임 만들지 않고 슬롯 수만큼 미리 만들어 두고
	// CUDA interop 으로 등록해 재사용한다.

	const bool resizeRequired = (m_cachedTextureWidth != m_videoFormatDesc.lumaWidth) ||
		(m_cachedTextureHeight != m_videoFormatDesc.lumaHeight);

	// 모든 슬롯이 갖춰져 있는지 확인한다. 0 번만 보면 부분 실패 상태를 놓친다.
	bool allSlotsReady = true;
	for (uint32_t slot = 0; slot < m_outputSlotCount; ++slot)
	{
		if (!m_outputTextures[slot] || !m_cudaResources[slot])
		{
			allSlotsReady = false;
			break;
		}
	}

	if (!resizeRequired && allSlotsReady)
	{
		return true;
	}

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return false;
	}

	DestroyOutputSlots();
	m_cachedTextureWidth = m_videoFormatDesc.lumaWidth;
	m_cachedTextureHeight = m_videoFormatDesc.lumaHeight;

	for (uint32_t slot = 0; slot < m_outputSlotCount; ++slot)
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
		desc.MiscFlags = m_config.sharedOutputTextureMode ? D3D11_RESOURCE_MISC_SHARED : 0;

		// CreateTexture2D 는 디바이스 호출이라 스레드 세이프하다.
		HRESULT hr = m_D3D11Device->CreateTexture2D(&desc, nullptr, &m_outputTextures[slot]);
		if (FAILED(hr))
		{
			printf_s("[NVDEC ERROR] CreateTexture2D failed for slot %u. hr=0x%08lX\n",
				slot, static_cast<unsigned long>(hr));
			DestroyOutputSlots();
			return false;
		}

		if (m_config.sharedOutputTextureMode)
		{
			IDXGIResource* dxgiResource = nullptr;
			hr = m_outputTextures[slot]->QueryInterface(
				__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgiResource));
			if (FAILED(hr) || !dxgiResource)
			{
				DestroyOutputSlots();
				return false;
			}

			hr = dxgiResource->GetSharedHandle(&m_frames[slot].sharedHandle);
			dxgiResource->Release();
			if (FAILED(hr) || !m_frames[slot].sharedHandle)
			{
				DestroyOutputSlots();
				return false;
			}
		}

		// CUDA interop 등록은 D3D11 리소스를 만진다. 게이트 안에서 처리한다.
		// 실패 정리(DestroyOutputSlots)가 스스로 게이트를 잡으므로
		// 이 범위를 벗어난 뒤에 호출해야 중첩 획득을 피할 수 있다.
		bool registered = false;
		{
			D3D11ImmediateContextGuard contextGuard(m_contextGate);
			registered = CUDA_DRVAPI_CALL(cuGraphicsD3D11RegisterResource(
				&m_cudaResources[slot],
				m_outputTextures[slot],
				CU_GRAPHICS_REGISTER_FLAGS_NONE));
		}

		if (!registered)
		{
			DestroyOutputSlots();
			return false;
		}

		m_frames[slot].texture = m_outputTextures[slot];
		m_frames[slot].timestamp = 0;
		m_frames[slot].slot = slot;
	}

	return true;
}

void D3D11NvDecoder_Impl::DestroyOutputSlots()
{
	// 생성 순서와 반대로 해제한다.
	// cuGraphicsUnregisterResource 는 D3D11 리소스를 만지므로 게이트가 필요하다.
	// 호출자가 게이트를 잡은 상태로 부르면 안 된다(재귀 획득 불가).
	D3D11ImmediateContextGuard contextGuard(m_contextGate);

	for (uint32_t slot = 0; slot < kMaxOutputSlotCount; ++slot)
	{
		if (m_cudaResources[slot])
		{
			CUDA_DRVAPI_CALL(cuGraphicsUnregisterResource(m_cudaResources[slot]));
			m_cudaResources[slot] = nullptr;
		}

		if (m_outputTextures[slot])
		{
			m_outputTextures[slot]->Release();
			m_outputTextures[slot] = nullptr;
		}

		m_frames[slot].texture = nullptr;
		m_frames[slot].sharedHandle = nullptr;
		m_frames[slot].timestamp = 0;
		m_frames[slot].slot = slot;
	}
}

bool D3D11NvDecoder_Impl::CreateBgraStagingBuffers()
{
	// BGRA 32 Bit 변환 결과가 저장될 Cuda Memory 생성

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return false;
	}

	DestroyBgraStagingBuffers();
	m_bgraStagingPitch = 0;

	for (uint32_t slot = 0; slot < m_outputSlotCount; ++slot)
	{
		// GPU 내부 메모리 구조에 맞추어 Aligned 된 메모리를 생성하기 위해
		// cuMemAllocPitch 사용.
		if (!CUDA_DRVAPI_CALL(cuMemAllocPitch(
			&m_bgraStagingBuffers[slot],
			&m_bgraStagingPitch,
			m_videoFormatDesc.lumaWidth * 4,
			m_videoFormatDesc.lumaHeight,
			16)))
		{
			DestroyBgraStagingBuffers();
			return false;
		}
	}

	return true;
}

void D3D11NvDecoder_Impl::DestroyBgraStagingBuffers()
{
	// Cuda Device 메모리 해제
	for (uint32_t slot = 0; slot < m_outputSlotCount; ++slot)
	{
		if (m_bgraStagingBuffers[slot])
		{
			CUDA_DRVAPI_CALL(cuMemFree(m_bgraStagingBuffers[slot]));
			m_bgraStagingBuffers[slot] = 0;
		}
	}
	m_bgraStagingPitch = 0;
}

void D3D11NvDecoder_Impl::WaitForAllSlots()
{
	// cuEvent 로 모든 프레임이 Idle 상태인지 체크
	// Decode 중 이라면 cuEventSynchronize 로 대기
	// HandlePictureDisplay 호출 종료 시점에 Event Set.

	ScopedCudaContext cudaContext(m_cudaContext);
	if (!cudaContext.IsActive())
	{
		return;
	}

	for (uint32_t slot = 0; slot < m_outputSlotCount; ++slot)
	{
		if (!m_decodeCompleteEvents[slot])
		{
			continue;
		}

		if (cuEventQuery(m_decodeCompleteEvents[slot]) == CUDA_SUCCESS)
		{
			continue;
		}

		CUDA_DRVAPI_CALL(cuEventSynchronize(m_decodeCompleteEvents[slot]));
	}
}

bool D3D11NvDecoder_Impl::SaveFrameToBmp(uint32_t slot, const wchar_t* fileName)
{
	if (slot >= m_outputSlotCount || !m_outputTextures[slot])
		return false;

	// 진단용. 컨텍스트를 직접 만지므로 게이트가 필요하다.
	D3D11ImmediateContextGuard contextGuard(m_contextGate);

	ID3D11Texture2D* gpuTexture = m_outputTextures[slot];
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
