#include "pch.h"
#include "D3D11NvDecoder.h"

#include <new>

#include "D3D11NvDecoder_Impl.h"

D3D11NvDecoder::D3D11NvDecoder()
	: m_impl(new (std::nothrow) D3D11NvDecoder_Impl())
{
}

D3D11NvDecoder::~D3D11NvDecoder()
{
	delete m_impl;
	m_impl = nullptr;
}

bool D3D11NvDecoder::Initialize(ID3D11Device* device)
{
	return m_impl && m_impl->Initialize(device);
}

void D3D11NvDecoder::ShutDown()
{
	if (m_impl)
	{
		m_impl->ShutDown();
	}
}

bool D3D11NvDecoder::Parse(const uint8_t* data, uint32_t size, bool endOfPicture, bool endOfStream, bool discontinuity)
{
	return m_impl && m_impl->Parse(data, size, endOfPicture, endOfStream, discontinuity);
}

D3D11NvDecoder::Frame* D3D11NvDecoder::GetFrame()
{
	if (!m_impl)
	{
		return nullptr;
	}

	D3D11NvDecoder_Impl::Frame* frame = m_impl->GetFrame();
	if (!frame)
	{
		return nullptr;
	}

	m_publicFrame.texture = frame->texture;
	m_publicFrame.timestamp = frame->timestamp;
	return &m_publicFrame;
}
