#pragma once

#include "../Nvidia Video Codec SDK/include/NvDecoder/nvcuvid.h"
#include "../Nvidia Video Codec SDK/include/NvDecoder/cuviddec.h"

#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define D3D11_NVIDIA_DECODER_API __declspec(dllexport)
#else
#define D3D11_NVIDIA_DECODER_API __declspec(dllimport)
#endif

struct ID3D11Texture2D;
struct ID3D11Device;
struct ID3D11DeviceContext;

class D3D11_NVIDIA_DECODER_API D3D11NvDecoder
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

	struct Frame
	{
		ID3D11Texture2D* texture = nullptr;
		uint64_t timestamp = 0;
	};

public:
	D3D11NvDecoder();
	~D3D11NvDecoder();

	bool Initialize(ID3D11Device* device);
	void ShutDown();

	bool Parse(const uint8_t* data, uint32_t size, bool endOfPicture = true, bool endOfStream = false, bool discontinuity = false);

	Frame* GetFrame();

private:

	static int32_t CUDAAPI HandleVideoSequence(void* userData, CUVIDEOFORMAT* format);
	static int32_t CUDAAPI HandlePictureDecode(void* userData, CUVIDPICPARAMS* pictureParams);
	static int32_t CUDAAPI HandlePictureDisplay(void* userData, CUVIDPARSERDISPINFO* displayInfo);

	int32_t OnVideoSequence(CUVIDEOFORMAT* format);
	int32_t OnPictureDecode(CUVIDPICPARAMS* pictureParams);
	int32_t OnPictureDisplay(CUVIDPARSERDISPINFO* displayInfo);

private:
	bool InitializeCuda();

	bool ReconfigureDecoder(CUVIDEOFORMAT* videoFormat);

	bool CreateTexturePool();
	void DestroyTexturePool();

	bool CreateCudaDeviceMemoryPool();
	void DestroyCudaDeviceMemoryPool();

	void WaitForAllFrames();

	bool SaveFrameToBmp(int32_t index, const wchar_t* fileName);
	bool SaveNV12ToRawFile(CUdeviceptr srcFrame, unsigned int srcPitch, const wchar_t* fileName);

private:
	ID3D11Device* m_D3D11Device = nullptr;
	ID3D11DeviceContext* m_D3D11Context = nullptr;

	CUdevice m_cudaDevice = 0;
	CUcontext m_cudaContext = nullptr;
	CUvideoctxlock m_ctxLock = nullptr;
	CUstream m_cuStream = nullptr;

	CUvideodecoder m_decoder = nullptr;
	CUvideoparser m_parser = nullptr;

	static const int32_t TEXTURE_POOL_COUNT = 8;
	static_assert((TEXTURE_POOL_COUNT > 0) && ((TEXTURE_POOL_COUNT& (TEXTURE_POOL_COUNT - 1)) == 0),
		"TEXTURE_POOL_COUNT must be a power of two");

	CUevent m_cuEvents[TEXTURE_POOL_COUNT] = {};
	ID3D11Texture2D* m_textures[TEXTURE_POOL_COUNT] = {};
	CUgraphicsResource m_cudaResources[TEXTURE_POOL_COUNT] = {};
	CUdeviceptr m_cuBGRABuffer[TEXTURE_POOL_COUNT] = {};
	size_t m_cuBGRAPitch = 0;

	CUVIDEOFORMAT m_cuVideoFormat;
	VideoFormatDesc m_videoFormatDesc = {};
	alignas(64) LONG m_reconfiguring = FALSE;

	uint32_t m_cacheTextureWidth = 0;
	uint32_t m_cacheTextureHeight = 0;
	Frame m_frames[TEXTURE_POOL_COUNT] = {};

	alignas(64) LONG m_writeIndex = 0;
	alignas(64) LONG m_readIndex = 0;
};

