#include "pch.h"
#include "EncodeCompletionThread.h"

#include "D3D11NvEncoder_Impl.h"

#include <stdio.h> // for printf_s

namespace
{
	// 비트스트림 회수 실패가 이만큼 연속되면 세션이 살아있다고 볼 수 없다.
	constexpr uint32_t kMaxConsecutiveLostFrames = 8U;
}

EncodeCompletionThread::EncodeCompletionThread()
	: Core::Concurrency::ThreadBase(L"EncodeCompletionThread")
{
}

EncodeCompletionThread::~EncodeCompletionThread()
{
	Shutdown();
}

bool EncodeCompletionThread::Initialize(D3D11NvEncoder_Impl* encoder)
{
	// 대기에 쓰는 이벤트는 엔코더가 소유한다.
	// 이 스레드와 수명을 묶으면 SubmitFrame 이 닫힌 핸들을 볼 수 있다.
	if (!encoder || !encoder->m_frameSubmittedEvent)
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

void EncodeCompletionThread::Run()
{
	D3D11NvEncoder_Impl* encoder = m_encoder;
	if (!encoder)
		return;

	HANDLE waitHandles[] = { GetStopEvent(), encoder->m_frameSubmittedEvent };

	// 단발성 프레임 유실로 스레드를 끝내면 파이프라인이 조용히 영구 정지한다.
	// 유실은 흘려보내고, 연속으로 누적될 때만 세션을 포기한다.
	uint32_t consecutiveLostFrames = 0;

	while (!IsStopRequested())
	{
		if (encoder->GetPendingFrameCount() == 0)
		{
			const DWORD waitResult = ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
			if (waitResult == WAIT_OBJECT_0)
				break;
			if (waitResult != WAIT_OBJECT_0 + 1)
			{
				printf_s("[NVENC ERROR] Encode completion thread wait failed. waitResult=%lu\n", waitResult);
				encoder->EnterFaultedState(NvEncErrorCode::EncoderFaulted);
				break;
			}
			continue;
		}

		const NvEncOutputResult result = encoder->ProcessOneOutput(true, true);
		if (result == NvEncOutputResult::Completed)
		{
			consecutiveLostFrames = 0;
			continue;
		}

		if (result == NvEncOutputResult::NotReady)
		{
			// pending > 0 인데 회수할 게 없는 상태. 정상 경로에서는 오지 않지만
			// 오더라도 바쁜 대기로 코어를 태우지 않도록 양보한다.
			::SwitchToThread();
			continue;
		}

		if (result == NvEncOutputResult::FrameLost)
		{
			// 슬롯은 회수됐으므로 계속 인코딩할 수 있다.
			if (++consecutiveLostFrames < kMaxConsecutiveLostFrames)
				continue;

			printf_s("[NVENC ERROR] %u consecutive frames lost. Giving up the session.\n",
				consecutiveLostFrames);
			encoder->EnterFaultedState(NvEncErrorCode::EncoderFaulted);
			break;
		}

		// Fatal. ProcessOneOutput 안에서 이미 faulted 상태로 진입했다.
		break;
	}
}
