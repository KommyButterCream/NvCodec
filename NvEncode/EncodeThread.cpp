#include "pch.h"
#include "EncodeThread.h"

#include <new>

#include "EncodeThread_Impl.h"

EncodeThread::EncodeThread()
	: m_impl(new (std::nothrow) EncodeThread_Impl())
{
}

EncodeThread::~EncodeThread()
{
	Shutdown();

	if (m_impl)
	{
		delete m_impl;
		m_impl = nullptr;
	}
}

bool EncodeThread::Initialize(EncodeFrameQueue* queue, D3D11NvEncoder* encoder)
{
	return m_impl && m_impl->Initialize(queue, encoder);
}

void EncodeThread::Shutdown()
{
	if (m_impl)
	{
		m_impl->Shutdown();
	}
}

void EncodeThread::SetEncodedFrameCallback(EncodedFrameCallback callback, void* userData)
{
	if (m_impl)
	{
		m_impl->SetEncodedFrameCallback(callback, userData);
	}
}

void EncodeThread::SetKeyFrameRequestCallback(KeyFrameRequestCallback callback, void* userData)
{
	if (m_impl)
	{
		m_impl->SetKeyFrameRequestCallback(callback, userData);
	}
}

void EncodeThread::GetStats(Stats& stats) const
{
	if (m_impl)
	{
		m_impl->GetStats(stats);
	}
	else
	{
		stats = Stats();
	}
}
