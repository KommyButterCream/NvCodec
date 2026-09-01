#include "pch.h"
#include "D3D11NvDecoder.h"

#include <new> // for std::nothrow

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

bool D3D11NvDecoder::Initialize(
	ID3D11Device* device,
	const NvDecConfig& config,
	ID3D11ImmediateContextGate* contextGate)
{
	return m_impl && m_impl->Initialize(device, config, contextGate);
}

bool D3D11NvDecoder::Initialize(
	ID3D11Device* device,
	ID3D11ImmediateContextGate* contextGate,
	bool sharedOutputTextureMode)
{
	NvDecConfig config;
	config.sharedOutputTextureMode = sharedOutputTextureMode;

	return Initialize(device, config, contextGate);
}

void D3D11NvDecoder::Destroy()
{
	if (m_impl)
	{
		m_impl->Destroy();
	}
}

bool D3D11NvDecoder::Parse(const uint8_t* data, uint32_t size, uint64_t timestamp,
	bool endOfPicture, bool endOfStream, bool discontinuity)
{
	return m_impl && m_impl->Parse(data, size, timestamp, endOfPicture, endOfStream, discontinuity);
}

D3D11NvDecoder::Frame* D3D11NvDecoder::AcquireFrame()
{
	// 프레임은 impl 이 슬롯별로 소유한다.
	// 예전에는 단일 m_publicFrame 에 복사해 돌려줬는데, 앱이 두 장을 동시에
	// 들고 있으면 두 번째가 첫 번째를 덮어써 버렸다.
	return m_impl ? m_impl->AcquireFrame() : nullptr;
}

void D3D11NvDecoder::ReleaseFrame(Frame* frame)
{
	if (m_impl)
	{
		m_impl->ReleaseFrame(frame);
	}
}

void D3D11NvDecoder::SetErrorCallback(ErrorCallback callback, void* userData)
{
	if (m_impl)
	{
		m_impl->SetErrorCallback(callback, userData);
	}
}

void D3D11NvDecoder::GetStats(NvDecStats& stats) const
{
	if (m_impl)
	{
		m_impl->GetStats(stats);
	}
	else
	{
		stats = NvDecStats();
	}
}

bool D3D11NvDecoder::IsFaulted() const
{
	return m_impl && m_impl->IsFaulted();
}
