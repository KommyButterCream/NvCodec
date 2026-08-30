#include "pch.h"
#include "EncodeCompletionThread.h"

#include "D3D11NvEncoder_Impl.h"

#include <stdio.h> // for printf_s

EncodeCompletionThread::EncodeCompletionThread()
	: Core::Concurrency::ThreadBase(L"EncodeCompletionThread")
	, m_frameSubmittedEvent(::CreateEvent(nullptr, FALSE, FALSE, nullptr))
{
}

EncodeCompletionThread::~EncodeCompletionThread()
{
	Shutdown();

	if (m_frameSubmittedEvent)
	{
		::CloseHandle(m_frameSubmittedEvent);
		m_frameSubmittedEvent = nullptr;
	}
}

bool EncodeCompletionThread::Initialize(D3D11NvEncoder_Impl* encoder)
{
	if (!encoder || !m_frameSubmittedEvent)
		return false;

	Shutdown();
	m_encoder = encoder;
	if (!Start())
	{
		m_encoder = nullptr;
		return false;
	}

	return true;
}

void EncodeCompletionThread::Shutdown()
{
	Stop();
	m_encoder = nullptr;
}

void EncodeCompletionThread::NotifyFrameSubmitted()
{
	if (m_frameSubmittedEvent)
		::SetEvent(m_frameSubmittedEvent);
}

void EncodeCompletionThread::Run()
{
	HANDLE waitHandles[] = { GetStopEvent(), m_frameSubmittedEvent };

	while (!IsStopRequested())
	{
		D3D11NvEncoder_Impl* encoder = m_encoder;
		if (!encoder)
			break;

		if (encoder->GetPendingFrameCount() == 0)
		{
			const DWORD waitResult = ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
			if (waitResult == WAIT_OBJECT_0)
				break;
			if (waitResult != WAIT_OBJECT_0 + 1)
			{
				printf_s("[NVENC ERROR] Encode completion thread wait failed. waitResult=%lu\n", waitResult);
				break;
			}
			continue;
		}

		if (!encoder->ProcessNextOutput(true, true))
		{
			printf_s("[NVENC ERROR] Encode completion thread failed to process output.\n");
			encoder->SignalAllSlotsFree();
			break;
		}
	}
}
