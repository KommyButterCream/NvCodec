#pragma once

#include "NvEncPacket.h"

#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define D3D11_NVIDIA_ENCODER_API __declspec(dllexport)
#else
#define D3D11_NVIDIA_ENCODER_API __declspec(dllimport)
#endif

struct ID3D11Device;
struct ID3D11Texture2D;
class D3D11NvEncoder_Impl;

class D3D11_NVIDIA_ENCODER_API D3D11NvEncoder
{
public:
	D3D11NvEncoder();
	~D3D11NvEncoder();

	D3D11NvEncoder(const D3D11NvEncoder&) = delete;
	D3D11NvEncoder& operator=(const D3D11NvEncoder&) = delete;

	bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t encodeBufferCount);
	void Destroy();

	bool PrepareFrameForEncode(ID3D11Texture2D* bgraTexture);
	bool DoEncode(NvEncPacket& encodeResultPacket);

private:
	D3D11NvEncoder_Impl* m_impl = nullptr;
};
