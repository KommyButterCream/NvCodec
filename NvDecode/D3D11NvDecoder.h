#pragma once

#include <cstdint>

#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define D3D11_NVIDIA_DECODER_API __declspec(dllexport)
#else
#define D3D11_NVIDIA_DECODER_API __declspec(dllimport)
#endif

struct ID3D11Texture2D;
struct ID3D11Device;
class D3D11NvDecoder_Impl;

class D3D11_NVIDIA_DECODER_API D3D11NvDecoder
{
public:
	struct Frame
	{
		ID3D11Texture2D* texture = nullptr;
		uint64_t timestamp = 0;
	};

	D3D11NvDecoder();
	~D3D11NvDecoder();

	D3D11NvDecoder(const D3D11NvDecoder&) = delete;
	D3D11NvDecoder& operator=(const D3D11NvDecoder&) = delete;

	bool Initialize(ID3D11Device* device);
	void ShutDown();

	bool Parse(const uint8_t* data, uint32_t size, bool endOfPicture = true, bool endOfStream = false, bool discontinuity = false);

	Frame* GetFrame();

private:
	D3D11NvDecoder_Impl* m_impl = nullptr;
	Frame m_publicFrame = {};
};
