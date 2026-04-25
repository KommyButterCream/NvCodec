#pragma once

#include <dxgiformat.h>

#include "../Nvidia Video Codec SDK/Interface/nvEncodeAPI.h"
#include "NvEncPacket.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
class D3D11VideoProcessorNV12;

struct NvEncInputFrame
{
	void* inputPtr = nullptr;
	uint32_t chromaOffsets[2] = { 0, 0 };
	uint32_t numChromaPlanes = 0;
	uint32_t pitch = 0;
	uint32_t chromaPitch = 0;
	NV_ENC_BUFFER_FORMAT bufferFormat = NV_ENC_BUFFER_FORMAT::NV_ENC_BUFFER_FORMAT_NV12;
	NV_ENC_INPUT_RESOURCE_TYPE resourceType = NV_ENC_INPUT_RESOURCE_TYPE::NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
};

struct NvEncOutputFrame
{
	uint8_t* streamData = nullptr;
	uint32_t streamDataSize = 0;
	uint32_t streamDataCapacity = 0;
	NV_ENC_PIC_TYPE pictureType = NV_ENC_PIC_TYPE::NV_ENC_PIC_TYPE_UNKNOWN;
	uint64_t timeStamp = 0;
	bool isKeyFrame = false;
};

class D3D11NvEncoder_Impl
{
public:
	D3D11NvEncoder_Impl() = default;
	~D3D11NvEncoder_Impl();

	D3D11NvEncoder_Impl(const D3D11NvEncoder_Impl&) = delete;
	D3D11NvEncoder_Impl& operator=(const D3D11NvEncoder_Impl&) = delete;

	bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t encodeBufferCount);
	void Destroy();

	bool PrepareFrameForEncode(ID3D11Texture2D* bgraTexture);
	bool DoEncode(NvEncPacket& encodeResultPacket);

private:
	bool LoadNvEncApi();
	bool OpenEncodeSession();

	bool InitializeEncoder();
	void DestroyEncoder();

	bool InitializeAsyncEvent();
	void DestoryAsyncEvent();

	bool InitializeMappedInputBuffers();
	void DestoryMappedInputBuffers();

	bool InitializeBitstreamBuffers();
	void DestoryBitstreamBuffers();

	bool InitializeRegisteredResources();
	void DestroyRegisteredResources();

	bool InitializeD3D11InputBuffers();
	void DestoryD3D11InputBuffers();

	bool InitializeBGRAtoNV12Converter();
	void DestroyBGRAtoNV12Converter();

	bool InitializeOutputFrameBuffers();
	void DestroyOutputFrameBuffers();
	void ReleaseOutputFrameBuffer(NvEncOutputFrame& frame);

	bool RegisterResource(void* buffer, NV_ENC_INPUT_RESOURCE_TYPE eResourceType,
		uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT eBufferFormat, NV_ENC_BUFFER_USAGE eBufferUsage,
		NV_ENC_REGISTERED_PTR& registeredResource);

	bool RegisterInputResources(void** inputFrames, uint32_t inputFrameCount, NV_ENC_INPUT_RESOURCE_TYPE eResourceType,
		uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT eBufferFormat);

	bool SetNV12OutputTexture(ID3D11Texture2D** textures, uint32_t bufferCount);
	bool SetBGRAInputTexture(ID3D11Texture2D** textures, uint32_t bufferCount);

	const NvEncInputFrame* GetNextInputFrame();
	uint32_t GetNextInputFrameIndex() const;
	uint32_t GetNextOutputFrameIndex() const;

	bool MapInputResources(uint32_t index);
	bool UnmapInputResources(uint32_t index);

	bool EncodeFrame(uint32_t index, bool& needsMoreInput);
	bool WaitForCompletionEvent(uint32_t index);
	bool GetEncodedPacket(uint32_t index, NvEncPacket& packet);
	bool Flush();

	int32_t GetCapabilityValue(GUID guidCodec, NV_ENC_CAPS capsToQuery);
	uint32_t GetEncodeWidth() const;
	uint32_t GetEncodeHeight() const;
	uint32_t GetMaxEncodeWidth() const;
	uint32_t GetMaxEncodeHeight() const;
	NV_ENC_BUFFER_FORMAT GetPixelFormat() const;
	DXGI_FORMAT GetD3D11Format(NV_ENC_BUFFER_FORMAT eBufferFormat) const;
	HANDLE GetCompletionEvent(uint32_t index);

private:
	ID3D11Device* m_D3D11Device = nullptr;
	ID3D11DeviceContext* m_D3D11Context = nullptr;

	void* m_encoderHandle = nullptr;
	NV_ENCODE_API_FUNCTION_LIST m_nvenc = {};

	D3D11VideoProcessorNV12* m_converter = nullptr;

	NV_ENC_INITIALIZE_PARAMS m_initParameters = {};
	NV_ENC_CONFIG m_config = {};

	HANDLE* m_completionEvent = nullptr;

	uint32_t m_encodeBufferCount = 1;
	NvEncInputFrame* m_inputFrames = nullptr;
	NvEncOutputFrame* m_outputFrames = nullptr;
	NV_ENC_REGISTERED_PTR* m_registeredResources = nullptr;
	NV_ENC_INPUT_PTR* m_mappedInputBuffers = nullptr;
	NV_ENC_OUTPUT_PTR* m_bitstreamBuffers = nullptr;

	ID3D11Texture2D** m_bgraTextures = nullptr;
	ID3D11Texture2D** m_nv12Textures = nullptr;

	uint64_t m_timeStamp = 0;
	uint32_t m_inputFrameIndex = 0;
	uint32_t m_outputFrameIndex = 0;

	uint32_t m_width = 0;
	uint32_t m_height = 0;
};
