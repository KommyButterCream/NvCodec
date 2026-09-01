#include "pch.h"
#include "D3D11NvDecoder.h"

#include <new> // for std::nothrow

#include "D3D11NvDecoder_Impl.h"

D3D11NvDecoder::D3D11NvDecoder()
	: m_impl(new (std::nothrow) D3D11NvDecoder_Impl())
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
	return m_impl->Initialize(device, config, contextGate);
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
	m_impl->Destroy();
}

bool D3D11NvDecoder::Parse(const uint8_t* data, uint32_t size, uint64_t timestamp,
	bool endOfPicture, bool endOfStream, bool discontinuity)
{
	return m_impl->Parse(data, size, timestamp, endOfPicture, endOfStream, discontinuity);
}

D3D11NvDecoder::Frame* D3D11NvDecoder::AcquireFrame()
{
	// 프레임은 impl 이 슬롯별로 소유한다.
	// 예전에는 단일 m_publicFrame 에 복사해 돌려줬는데, 앱이 두 장을 동시에
	// 들고 있으면 두 번째가 첫 번째를 덮어써 버렸다.
	return m_impl->AcquireFrame();
}

void D3D11NvDecoder::ReleaseFrame(Frame* frame)
{
	m_impl->ReleaseFrame(frame);
}

void D3D11NvDecoder::SetErrorCallback(ErrorCallback callback, void* userData)
{
	m_impl->SetErrorCallback(callback, userData);
}

bool D3D11NvDecoder::StartDecodeThread(DecodeFrameQueue* queue)
{
	return m_impl->StartDecodeThread(queue);
}

void D3D11NvDecoder::StopDecodeThread()
{
	m_impl->StopDecodeThread();
}

void D3D11NvDecoder::SetFrameCallback(FrameCallback callback, void* userData)
{
	m_impl->SetFrameCallback(callback, userData);
}

void D3D11NvDecoder::GetStats(NvDecStats& stats) const
{
	m_impl->GetStats(stats);
}

bool D3D11NvDecoder::IsFaulted() const
{
	return m_impl->IsFaulted();
}
