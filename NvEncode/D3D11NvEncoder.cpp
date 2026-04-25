#include "pch.h"
#include "D3D11NvEncoder.h"

#include <new>

#include "D3D11NvEncoder_Impl.h"

D3D11NvEncoder::D3D11NvEncoder()
	: m_impl(new (std::nothrow) D3D11NvEncoder_Impl())
{
}

D3D11NvEncoder::~D3D11NvEncoder()
{
	delete m_impl;
	m_impl = nullptr;
}

bool D3D11NvEncoder::Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t encodeBufferCount)
{
	return m_impl && m_impl->Initialize(device, width, height, encodeBufferCount);
}

void D3D11NvEncoder::Destroy()
{
	if (m_impl)
	{
		m_impl->Destroy();
	}
}

bool D3D11NvEncoder::PrepareFrameForEncode(ID3D11Texture2D* bgraTexture)
{
	return m_impl && m_impl->PrepareFrameForEncode(bgraTexture);
}

bool D3D11NvEncoder::DoEncode(NvEncPacket& encodeResultPacket)
{
	return m_impl && m_impl->DoEncode(encodeResultPacket);
}
