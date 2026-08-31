#include "pch.h"
#include "D3D11NvEncoder.h"

#include <new> // for std::nothrow

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

bool D3D11NvEncoder::Initialize(
	ID3D11Device* device,
	uint32_t width,
	uint32_t height,
	uint32_t encodeBufferCount,
	ID3D11ImmediateContextGate* contextGate,
	bool enableAsyncPipeline)
{
	return m_impl && m_impl->Initialize(device, width, height, encodeBufferCount, contextGate, enableAsyncPipeline);
}

void D3D11NvEncoder::Destroy()
{
	if (m_impl)
	{
		m_impl->Destroy();
	}
}

void D3D11NvEncoder::SetEncodedPacketCallback(EncodedPacketCallback callback, void* userData)
{
	if (m_impl)
	{
		m_impl->SetEncodedPacketCallback(callback, userData);
	}
}

void D3D11NvEncoder::SetErrorCallback(ErrorCallback callback, void* userData)
{
	if (m_impl)
	{
		m_impl->SetErrorCallback(callback, userData);
	}
}

bool D3D11NvEncoder::PrepareFrameForEncode(ID3D11Texture2D* bgraTexture)
{
	return m_impl && m_impl->PrepareFrameForEncode(bgraTexture);
}

void D3D11NvEncoder::RequestKeyFrame()
{
	if (m_impl)
	{
		m_impl->RequestKeyFrame();
	}
}

bool D3D11NvEncoder::CanSubmitFrame() const
{
	return m_impl && m_impl->CanSubmitFrame();
}

bool D3D11NvEncoder::SubmitFrame(uint64_t frameId)
{
	return m_impl && m_impl->SubmitFrame(frameId);
}

uint32_t D3D11NvEncoder::GetPendingFrameCount() const
{
	return m_impl ? m_impl->GetPendingFrameCount() : 0;
}

bool D3D11NvEncoder::WaitForPendingFrames(uint32_t timeoutMilliseconds) const
{
	return m_impl && m_impl->WaitForPendingFrames(timeoutMilliseconds);
}

bool D3D11NvEncoder::IsAsyncPipelineEnabled() const
{
	return m_impl && m_impl->IsAsyncPipelineEnabled();
}

bool D3D11NvEncoder::DoEncode(NvEncPacket& encodeResultPacket)
{
	return m_impl && m_impl->DoEncode(encodeResultPacket);
}

bool D3D11NvEncoder::IsFaulted() const
{
	return m_impl && m_impl->IsFaulted();
}

void D3D11NvEncoder::GetStats(NvEncStats& stats) const
{
	if (m_impl)
	{
		m_impl->GetStats(stats);
	}
	else
	{
		stats = NvEncStats();
	}
}

void D3D11NvEncoder::DebugFailNextOutputs(uint32_t count)
{
	if (m_impl)
	{
		m_impl->DebugFailNextOutputs(count);
	}
}
