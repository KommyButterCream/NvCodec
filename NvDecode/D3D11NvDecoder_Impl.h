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
class DecodePacketQueue;
class DecodeThread;

class D3D11NvDecoder_Impl
{
public:
	struct VideoFormatDesc
	{
		bool isInitialized = false;

		cudaVideoCodec codec = cudaVideoCodec::cudaVideoCodec_H264;
		cudaVideoChromaFormat chromaFormat = cudaVideoChromaFormat::cudaVideoChromaFormat_420;
		cudaVideoSurfaceFormat outputFormat = cudaVideoSurfaceFormat::cudaVideoSurfaceFormat_NV12;
		cudaVideoDeinterlaceMode interlaceMode = cudaVideoDeinterlaceMode::cudaVideoDeinterlaceMode_Weave;

		uint8_t bitDepthMinus8 = 0;
		uint32_t bitsPerPixel = 0;

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

	// =====================================================================
	// 생성 / 소멸
	// =====================================================================
	D3D11NvDecoder_Impl() = default;
	~D3D11NvDecoder_Impl();

	D3D11NvDecoder_Impl(const D3D11NvDecoder_Impl&) = delete;
	D3D11NvDecoder_Impl& operator=(const D3D11NvDecoder_Impl&) = delete;

	// =====================================================================
	// 초기화 / 종료
	// =====================================================================
	bool Initialize(
		ID3D11Device* device,
		const NvDecConfig& config,
		ID3D11ImmediateContextGate* contextGate);
	void Destroy();

	// =====================================================================
	// 디코드 스레드 제어
	// =====================================================================
	bool StartDecodeThread();
	void StopDecodeThread();

	// 유입. 디코드 스레드가 꺼내 간다.
	bool EnqueuePacket(const NvDecPacket& packet);

	// =====================================================================
	// 콜백 등록
	// =====================================================================
	void SetFrameCallback(D3D11NvDecoder::FrameCallback callback, void* userData);
	void SetErrorCallback(ErrorCallback callback, void* userData);

	// =====================================================================
	// 비트스트림 투입
	// =====================================================================
	bool Parse(const uint8_t* data, uint32_t size, uint64_t timestamp,
		bool endOfPicture, bool endOfStream, bool discontinuity);

	// =====================================================================
	// 프레임 수신
	// =====================================================================
	Frame* AcquireFrame();
	void ReleaseFrame(Frame* frame);

	// =====================================================================
	// 통계 / 상태 조회
	// =====================================================================
	void GetStats(NvDecStats& stats) const;
	bool IsFaulted() const;

private:
	// --- 파서 콜백 (NVDEC) ---
	static int32_t CUDAAPI VideoSequenceCallback(void* userData, CUVIDEOFORMAT* format);
	static int32_t CUDAAPI PictureDecodeCallback(void* userData, CUVIDPICPARAMS* pictureParams);
	static int32_t CUDAAPI PictureDisplayCallback(void* userData, CUVIDPARSERDISPINFO* displayInfo);

	int32_t OnVideoSequence(CUVIDEOFORMAT* format);
	int32_t OnPictureDecode(CUVIDPICPARAMS* pictureParams);
	int32_t OnPictureDisplay(CUVIDPARSERDISPINFO* displayInfo);

	// --- 초기화 / 리소스 생성 ---
	bool CreateCudaResources();

	bool CreateOutputSlots();
	void DestroyOutputSlots();

	bool CreateBGRAStagingBuffers();
	void DestroyBGRAStagingBuffers();

	bool CreateInputQueue(size_t depth, size_t bufferSize);
	void DestroyInputQueue();

	void WaitForAllSlotGpuWork();

	// --- 재설정 ---
	// 반환값은 OnVideoSequence 가 파서에 그대로 돌려줄 값이다.
	// SequenceResult::Failed 면 실패, 그 외에는 decode surface 수.
	int32_t ReconfigureForVideoFormat(CUVIDEOFORMAT* videoFormat);

	// --- 오류 / 콜백 통지 ---
	void EnterFaultedState(NvDecErrorCode errorCode);
	void InvokeErrorCallback(NvDecErrorCode errorCode);
	void RecordLostFrame(NvDecErrorCode errorCode);
	void ResetLostFrameStreak();

	// --- 조회 ---
	// 출력 슬롯 링의 두 커서. Input 은 다음에 쓸 슬롯이고 — 앱이 들고 있으면
	// 쓸 수 없다 — Output 은 AcquireFrame 이 다음에 꺼낼 슬롯이다.
	uint32_t GetInputSlotIndex() const;
	uint32_t GetOutputSlotIndex() const;
	bool IsSlotHeldByApp(uint32_t slot) const;

private:
	// 출력 슬롯. 하나의 슬롯 번호가 아래를 전부 색인한다.
	// 엔코더의 슬롯 링과 같은 구조다.
	static constexpr uint32_t kMaxOutputSlotCount = 32;
	static constexpr uint32_t kMinOutputSlotCount = 2;

	// =====================================================================
	// 초기화 시점에 정해지고 이후 바뀌지 않는 것들
	// =====================================================================
	ID3D11Device* m_D3D11Device = nullptr;
	ID3D11DeviceContext* m_D3D11Context = nullptr;
	ID3D11ImmediateContextGate* m_contextGate = nullptr;

	CUdevice m_cudaDevice = 0;
	CUcontext m_cudaContext = nullptr;
	CUvideoctxlock m_videoContextLock = nullptr;
	CUstream m_cudaStream = nullptr;

	CUvideodecoder m_decoderHandle = nullptr;
	CUvideoparser m_parserHandle = nullptr;

	// 앱이 준 설정 원본.
	NvDecConfig m_userConfig = {};

	// =====================================================================
	// 출력 슬롯 리소스
	// =====================================================================
	uint32_t m_outputSlotCount = 0;
	CUevent m_decodeCompleteEvents[kMaxOutputSlotCount] = {};
	ID3D11Texture2D* m_outputTextures[kMaxOutputSlotCount] = {};
	CUgraphicsResource m_cudaOutputResources[kMaxOutputSlotCount] = {};
	CUdeviceptr m_bgraStagingBuffers[kMaxOutputSlotCount] = {};
	Frame m_outputFrames[kMaxOutputSlotCount] = {};

	size_t m_bgraStagingPitch = 0;

	uint32_t m_outputTextureWidth = 0;
	uint32_t m_outputTextureHeight = 0;

	// NVDEC 가 준 원본 포맷과, 그것을 이 클래스가 쓰기 좋게 풀어 둔 사본.
	// 스트림 포맷이 바뀌었는지는 원본끼리 비교해서 판단한다.
	CUVIDEOFORMAT m_currentVideoFormat = {};
	VideoFormatDesc m_videoFormatDesc = {};

	// =====================================================================
	// 디코드 스레드 / 콜백
	// =====================================================================

	// 큐 펌프. 앱이 StartDecodeThread 를 부를 때만 생성된다.
	// 직접 Parse / AcquireFrame 을 돌리는 앱에서는 nullptr 로 남는다.
	DecodeThread* m_decodeThread = nullptr;

	// 유입 큐. 소비자가 이 디코더 하나뿐이라 디코더가 소유한다.
	DecodePacketQueue* m_inputQueue = nullptr;

	// StartDecodeThread 이전에 SetFrameCallback 이 불릴 수 있다.
	D3D11NvDecoder::FrameCallback m_frameCallback = nullptr;
	void* m_frameCallbackUserData = nullptr;

	ErrorCallback m_errorCallback = nullptr;
	void* m_errorCallbackUserData = nullptr;
	mutable SRWLOCK m_callbackLock = SRWLOCK_INIT;

	// =====================================================================
	// 생산 측 — 파싱 / 표시 경로가 쓴다
	// 래핑하지 않는 단조증가 카운터. 슬롯은 & (count - 1) 로 얻는다.
	// =====================================================================
	alignas(64) volatile LONG m_inputSequence = 0;
	volatile LONG64 m_parsedPacketCount = 0;
	volatile LONG64 m_decodedFrameCount = 0;
	volatile LONG64 m_droppedPoolExhaustedCount = 0;

	// =====================================================================
	// 소비 측 — AcquireFrame / ReleaseFrame 이 쓴다
	// =====================================================================
	alignas(64) volatile LONG m_outputSequence = 0;
	volatile LONG m_framesHeldByApp = 0;
	volatile LONG64 m_deliveredFrameCount = 0;

	// =====================================================================
	// 양쪽이 함께 갱신하는 것
	// =====================================================================
	alignas(64) volatile LONG64 m_droppedNotConsumedCount = 0;
	volatile LONG64 m_droppedDisplayFailedCount = 0;

	// =====================================================================
	// 읽기 위주 상태
	// =====================================================================
	alignas(64) volatile LONG m_faulted = FALSE;
	volatile LONG m_consecutiveLostFrames = 0;
	volatile LONG m_reconfiguring = FALSE;

	// 앱이 AcquireFrame 으로 가져간 뒤 아직 ReleaseFrame 하지 않은 슬롯.
	// 이 슬롯에는 새 프레임을 쓸 수 없다.
	alignas(64) volatile LONG m_slotHeldByApp[kMaxOutputSlotCount] = {};
};
