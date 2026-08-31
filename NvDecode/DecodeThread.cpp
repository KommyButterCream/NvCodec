#include "pch.h"
#include "DecodeThread.h"

#include <new> // for std::nothrow

#include "DecodeThread_Impl.h"

DecodeThread::DecodeThread()
	: m_impl(new (std::nothrow) DecodeThread_Impl())
{
}

DecodeThread::~DecodeThread()
{
	Shutdown();

	if (m_impl)
	{
		delete m_impl;
		m_impl = nullptr;
	}
}

bool DecodeThread::Initialize(DecodeFrameQueue* queue, D3D11NvDecoder* decoder)
{
	return m_impl && m_impl->Initialize(queue, decoder);
}

void DecodeThread::Shutdown()
{
	if (m_impl)
	{
		m_impl->Shutdown();
	}
}

void DecodeThread::SetFrameCallback(FrameCallback callback, void* userData)
{
	if (m_impl)
	{
		m_impl->SetFrameCallback(callback, userData);
	}
}
