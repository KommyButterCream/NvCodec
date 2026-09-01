#include "pch.h"
#include "D3D11NvEncoder.h"

#include <new> // for std::nothrow

#include "D3D11NvEncoder_Impl.h"

D3D11NvEncoder::D3D11NvEncoder()
	: m_impl(new (std::nothrow) D3D11NvEncoder_Impl())
{
	// m_impl 은 이 시점 이후로 절대 null 이 아니다. 그게 클래스 불변식이고,
	// 그래서 아래 전달 메서드들이 매번 확인하지 않는다.
	//
	// 수백 바이트 할당이 실패했다면 프로세스는 이미 복구 불가능이다.
	// 조용히 넘어가고 모든 호출을 no-op 로 만들면 원인은 사라지고
	// "아무 일도 일어나지 않는" 증상만 남는다. 여기서 즉시 죽는 편이 낫다.
	if (!m_impl)
	{
		::RaiseFailFastException(nullptr, nullptr, 0);
	}
}

D3D11NvEncoder::~D3D11NvEncoder()
{
	delete m_impl;
	m_impl = nullptr;
}

bool D3D11NvEncoder::Initialize(
	ID3D11Device* device,
	const NvEncConfig& config,
	ID3D11ImmediateContextGate* contextGate)
{
	return m_impl->Initialize(device, config, contextGate);
}

bool D3D11NvEncoder::Initialize(
	ID3D11Device* device,
	uint32_t width,
	uint32_t height,
	uint32_t encodeBufferCount,
	ID3D11ImmediateContextGate* contextGate,
	bool enableAsyncPipeline)
{
	NvEncConfig config;
	config.width = width;
	config.height = height;
	config.encodeBufferCount = encodeBufferCount;
	config.enableAsyncPipeline = enableAsyncPipeline;

	return Initialize(device, config, contextGate);
}

NvEncReconfigureResult D3D11NvEncoder::Reconfigure(const NvEncConfig& config, bool forceIdr)
{
	return m_impl
		? m_impl->Reconfigure(config, forceIdr)
		: NvEncReconfigureResult::NotInitialized;
}

void D3D11NvEncoder::GetConfig(NvEncConfig& config) const
{
	m_impl->GetConfig(config);
}

void D3D11NvEncoder::Destroy()
{
	m_impl->Destroy();
}

void D3D11NvEncoder::SetEncodedPacketCallback(EncodedPacketCallback callback, void* userData)
{
	m_impl->SetEncodedPacketCallback(callback, userData);
}

void D3D11NvEncoder::SetErrorCallback(ErrorCallback callback, void* userData)
{
	m_impl->SetErrorCallback(callback, userData);
}

bool D3D11NvEncoder::PrepareFrameForEncode(ID3D11Texture2D* bgraTexture)
{
	return m_impl->PrepareFrameForEncode(bgraTexture);
}

void D3D11NvEncoder::RequestKeyFrame()
{
	m_impl->RequestKeyFrame();
}

bool D3D11NvEncoder::CanSubmitFrame() const
{
	return m_impl->CanSubmitFrame();
}

bool D3D11NvEncoder::SubmitFrame(uint64_t frameId)
{
	return m_impl->SubmitFrame(frameId);
}

uint32_t D3D11NvEncoder::GetPendingFrameCount() const
{
	return m_impl->GetPendingFrameCount();
}

bool D3D11NvEncoder::WaitForPendingFrames(uint32_t timeoutMilliseconds) const
{
	return m_impl->WaitForPendingFrames(timeoutMilliseconds);
}

bool D3D11NvEncoder::IsAsyncPipelineEnabled() const
{
	return m_impl->IsAsyncPipelineEnabled();
}

bool D3D11NvEncoder::DoEncode(NvEncPacket& encodeResultPacket)
{
	return m_impl->DoEncode(encodeResultPacket);
}

bool D3D11NvEncoder::IsFaulted() const
{
	return m_impl->IsFaulted();
}

void D3D11NvEncoder::GetStats(NvEncStats& stats) const
{
	m_impl->GetStats(stats);
}

bool D3D11NvEncoder::StartEncodeThread(EncodeFrameQueue* queue)
{
	return m_impl->StartEncodeThread(queue);
}

void D3D11NvEncoder::StopEncodeThread()
{
	m_impl->StopEncodeThread();
}

void D3D11NvEncoder::SetKeyFrameRequestCallback(KeyFrameRequestCallback callback, void* userData)
{
	m_impl->SetKeyFrameRequestCallback(callback, userData);
}

void D3D11NvEncoder::DebugFailNextOutputs(uint32_t count)
{
	m_impl->DebugFailNextOutputs(count);
}
