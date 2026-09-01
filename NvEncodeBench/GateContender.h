#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <process.h>
#include <stdint.h>

#include "LatencyStats.h"
#include "../../D3D11EngineInterface/ID3D11ImmediateContextGate.h"

namespace Bench
{
	// 게이트를 주기적으로 빼앗는 가짜 렌더 스레드.
	//
	// 실제 앱에서는 렌더 스레드가 같은 즉시 컨텍스트를 쓰므로,
	// 엔코더나 디코더는 항상 게이트를 기다리는 상태에 놓인다.
	// 경합이 없는 벤치 수치는 실사용을 대변하지 못한다.
	class GateContender
	{
	public:
		GateContender(ID3D11ImmediateContextGate* gate, uint32_t hz, uint32_t holdMicroseconds)
			: m_gate(gate)
			, m_holdMicroseconds(holdMicroseconds)
		{
			if (!m_gate || hz == 0 || holdMicroseconds == 0)
				return;

			m_intervalTicks = static_cast<int64_t>(QpcTicksPerSecond() / static_cast<double>(hz));
			m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!m_stopEvent)
				return;

			m_thread = reinterpret_cast<HANDLE>(
				::_beginthreadex(nullptr, 0, &GateContender::ThreadProc, this, 0, nullptr));
		}

		~GateContender()
		{
			if (m_stopEvent)
				::SetEvent(m_stopEvent);

			if (m_thread)
			{
				::WaitForSingleObject(m_thread, INFINITE);
				::CloseHandle(m_thread);
				m_thread = nullptr;
			}

			if (m_stopEvent)
			{
				::CloseHandle(m_stopEvent);
				m_stopEvent = nullptr;
			}
		}

		GateContender(const GateContender&) = delete;
		GateContender& operator=(const GateContender&) = delete;

		bool IsRunning() const { return m_thread != nullptr; }

		uint64_t GetAcquireCount() const
		{
			return static_cast<uint64_t>(
				::ReadAcquire64(&m_acquireCount));
		}

		// 게이트를 얻기까지 기다린 최대 시간. 상대가 게이트를 오래 잡으면 커진다.
		double GetMaxWaitMs() const
		{
			const LONG64 ticks =
				::ReadAcquire64(&m_maxWaitTicks);
			return TicksToMilliseconds(ticks);
		}

	private:
		static unsigned int __stdcall ThreadProc(void* arg)
		{
			static_cast<GateContender*>(arg)->Run();
			return 0;
		}

		void Run()
		{
			::SetThreadDescription(::GetCurrentThread(), L"BenchGateContender");

			int64_t nextDeadline = QpcNow();
			while (::WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0)
			{
				nextDeadline += m_intervalTicks;

				const int64_t waitStart = QpcNow();
				{
					D3D11ImmediateContextGuard guard(m_gate);
					const int64_t waited = QpcNow() - waitStart;
					if (waited > ::ReadAcquire64(&m_maxWaitTicks))
						::InterlockedExchange64(&m_maxWaitTicks, waited);

					::InterlockedIncrement64(&m_acquireCount);
					SpinForMicroseconds(m_holdMicroseconds);
				}

				const int64_t remaining = nextDeadline - QpcNow();
				if (remaining > 0)
				{
					const DWORD sleepMs = static_cast<DWORD>(TicksToMilliseconds(remaining));
					::WaitForSingleObject(m_stopEvent, sleepMs);
				}
				else
				{
					nextDeadline = QpcNow();
				}
			}
		}

	private:
		ID3D11ImmediateContextGate* m_gate = nullptr;
		uint32_t m_holdMicroseconds = 0;
		int64_t m_intervalTicks = 0;
		HANDLE m_thread = nullptr;
		HANDLE m_stopEvent = nullptr;
		alignas(8) volatile LONG64 m_acquireCount = 0;
		alignas(8) volatile LONG64 m_maxWaitTicks = 0;
	};
}
