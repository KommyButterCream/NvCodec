#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../Nvidia Video Codec SDK/include/NvDecoder/nvcuvid.h"
#include "../Nvidia Video Codec SDK/include/NvDecoder/cuviddec.h"

#include "D3D11NvDecoder.h"
#include "NvDecConfig.h"

struct ID3D11Texture2D;
struct ID3D11Device;
struct ID3D11DeviceContext;
class ID3D11ImmediateContextGate;
class DecodeFrameQueue;
class DecodeThread;

class D3D11NvDecoder_Impl
{
public:
	struct VideoFormatDesc
	{
		bool isInitialized = false;

		cudaVideoCodec eCodec = cudaVideoCodec::cudaVideoCodec_H264;
		cudaVideoChromaFormat eChromaFormat = cudaVideoChromaFormat::cudaVideoChromaFormat_420;
		cudaVideoSurfaceFormat eOutputFormat = cudaVideoSurfaceFormat::cudaVideoSurfaceFormat_NV12;
		cudaVideoDeinterlaceMode eInterlaceMode = cudaVideoDeinterlaceMode::cudaVideoDeinterlaceMode_Weave;

		uint8_t bitDepthMinus8 = 0;
		uint32_t bitPerPixel = 0;

		uint32_t codedWidth = 0;
		uint32_t codedHeight = 0;

		uint32_t maxCodedWidth = 0;
		uint32_t maxCodedHeight = 0;

		uint32_t lumaWidth = 0;
		uint32_t lumaHeight = 0;
		uint32_t chromaHeight = 0;
		uint32_t chromaPlanes = 0;

		uint32_t decodeSurfaceCount = 0;
	};

	// OnVideoSequence 가 파서에 돌려주는 값.
	// NVDEC 규약이라 의미를 이름으로 남긴다.
	//   0            : 실패. 파싱을 중단한다
	//   1            : 성공. 기존 decode surface 수를 그대로 쓴다
	//   2 이상       : 성공. 이 수만큼 decode surface 를 쓴다
	enum class SequenceResult : int32_t
	{
		Failed = 0,
		KeepSurfaceCount = 1,
	};

	using ErrorCallback = D3D11NvDecoder::ErrorCallback;
	using Frame = D3D11NvDecoder::Frame;

	D3D11NvDecoder_Impl() = default;
	~D3D11NvDecoder_Impl();

	D3D11NvDecoder_Impl(const D3D11NvDecoder_Impl&) = delete;
	D3D11NvDecoder_Impl& operator=(const D3D11NvDecoder_Impl&) = delete;

	bool Initialize(
		ID3D11Device* device,
		const NvDecConfig& config,
		ID3D11ImmediateContextGate* contextGate);
	void Destroy();

	bool Parse(const uint8_t* data, uint32_t size, uint64_t timestamp,
		bool endOfPicture, bool endOfStream, bool discontinuity);

	Frame* AcquireFrame();
	void ReleaseFrame(Frame* frame);

	void SetErrorCallback(ErrorCallback callback, void* userData);

	bool StartDecodeThread(DecodeFrameQueue* queue);
	void StopDecodeThread();
	void SetFrameCallback(D3D11NvDecoder::FrameCallback callback, void* userData);
	void GetStats(NvDecStats& stats) const;
	bool IsFaulted() const;

private:
	static int32_t CUDAAPI HandleVideoSequence(void* userData, CUVIDEOFORMAT* format);
	static int32_t CUDAAPI HandlePictureDecode(void* userData, CUVIDPICPARAMS* pictureParams);
	static int32_t CUDAAPI HandlePictureDisplay(void* userData, CUVIDPARSERDISPINFO* displayInfo);

	int32_t OnVideoSequence(CUVIDEOFORMAT* format);
	int32_t OnPictureDecode(CUVIDPICPARAMS* pictureParams);
	int32_t OnPictureDisplay(CUVIDPARSERDISPINFO* displayInfo);

	bool InitializeCuda();

	// 반환값은 OnVideoSequence 가 파서에 그대로 돌려줄 값이다.
	// SequenceResult::Failed 면 실패, 그 외에는 decode surface 수.
	int32_t ReconfigureDecoder(CUVIDEOFORMAT* videoFormat);

	bool CreateOutputSlots();
	void DestroyOutputSlots();

	bool CreateBgraStagingBuffers();
	void DestroyBgraStagingBuffers();

	void WaitForAllSlots();

	// FIFO 로 다음에 쓸 슬롯. 앱이 들고 있으면 쓸 수 없다.
	uint32_t GetWriteSlotIndex() const;
	uint32_t GetReadSlotIndex() const;
	bool IsSlotHeldByApp(uint32_t slot) const;

	void EnterFaultedState(NvDecErrorCode errorCode);
	void InvokeErrorCallback(NvDecErrorCode errorCode);
	void NoteLostFrame(NvDecErrorCode errorCode);
	void NoteHealthyFrame();

	bool SaveFrameToBmp(uint32_t slot, const wchar_t* fileName);
	bool SaveNV12ToRawFile(CUdeviceptr srcFrame, unsigned int srcPitch, const wchar_t* fileName);

private:
	ID3D11Device* m_D3D11Device = nullptr;
	ID3D11DeviceContext* m_D3D11Context = nullptr;
	ID3D11ImmediateContextGate* m_contextGate = nullptr;

	// 큐 펌프. 앱이 StartDecodeThread 를 부를 때만 생성된다.
	// 직접 Parse / AcquireFrame 을 돌리는 앱에서는 nullptr 로 남는다.
	DecodeThread* m_decodeThread = nullptr;

	// StartDecodeThread 이전에 SetFrameCallback 이 불릴 수 있다.
	D3D11NvDecoder::FrameCallback m_pendingFrameCallback = nullptr;
	void* m_pendingFrameCallbackUserData = nullptr;

	CUdevice m_cudaDevice = 0;
	CUcontext m_cudaContext = nullptr;
	CUvideoctxlock m_ctxLock = nullptr;
	CUstream m_cuStream = nullptr;

	CUvideodecoder m_decoder = nullptr;
	CUvideoparser m_parser = nullptr;

	// 출력 슬롯. 하나의 슬롯 번호가 아래를 전부 색인한다.
	// 엔코더의 슬롯 링과 같은 구조다.
	static constexpr uint32_t kMaxOutputSlotCount = 32;
	static constexpr uint32_t kMinOutputSlotCount = 2;

	uint32_t m_outputSlotCount = 0;
	CUevent m_decodeCompleteEvents[kMaxOutputSlotCount] = {};
	ID3D11Texture2D* m_outputTextures[kMaxOutputSlotCount] = {};
	CUgraphicsResource m_cudaResources[kMaxOutputSlotCount] = {};
	CUdeviceptr m_bgraStagingBuffers[kMaxOutputSlotCount] = {};
	Frame m_frames[kMaxOutputSlotCount] = {};

	// 앱이 AcquireFrame 으로 가져간 뒤 아직 ReleaseFrame 하지 않은 슬롯.
	// 이 슬롯에는 새 프레임을 쓸 수 없다.
	alignas(4) volatile LONG m_slotHeldByApp[kMaxOutputSlotCount] = {};

	size_t m_bgraStagingPitch = 0;

	CUVIDEOFORMAT m_cuVideoFormat = {};
	VideoFormatDesc m_videoFormatDesc = {};
	alignas(64) volatile LONG m_reconfiguring = FALSE;

	uint32_t m_cachedTextureWidth = 0;
	uint32_t m_cachedTextureHeight = 0;

	NvDecConfig m_config = {};

	// 래핑하지 않는 단조증가 카운터. 슬롯은 & (count - 1) 로 얻는다.
	alignas(64) volatile LONG m_writeSequence = 0;
	alignas(64) volatile LONG m_readSequence = 0;

	alignas(4) volatile LONG m_faulted = FALSE;
	alignas(4) volatile LONG m_consecutiveLostFrames = 0;
	alignas(4) volatile LONG m_framesHeldByApp = 0;
	alignas(8) volatile LONG64 m_parsedPacketCount = 0;
	alignas(8) volatile LONG64 m_decodedFrameCount = 0;
	alignas(8) volatile LONG64 m_deliveredFrameCount = 0;
	alignas(8) volatile LONG64 m_droppedPoolExhaustedCount = 0;
	alignas(8) volatile LONG64 m_droppedNotConsumedCount = 0;
	alignas(8) volatile LONG64 m_droppedDisplayFailedCount = 0;

	ErrorCallback m_errorCallback = nullptr;
	void* m_errorCallbackUserData = nullptr;
	mutable SRWLOCK m_callbackLock = SRWLOCK_INIT;
};
