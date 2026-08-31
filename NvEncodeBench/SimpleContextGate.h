#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../D3D11EngineInterface/ID3D11ImmediateContextGate.h"

namespace Bench
{
	// D3D11 Immediate Context 는 스레드 세이프하지 않다.
	// async 파이프라인에서는 EncodeThread(Prepare/Convert/Map/Encode)와
	// EncodeCompletionThread(Unmap)가 동시에 컨텍스트를 만지므로
	// 게이트를 반드시 넘겨야 한다.
	class SimpleContextGate final : public ID3D11ImmediateContextGate
	{
	public:
		SimpleContextGate()
		{
			::InitializeCriticalSectionAndSpinCount(&m_lock, 4000);
		}

		~SimpleContextGate() override
		{
			::DeleteCriticalSection(&m_lock);
		}

		SimpleContextGate(const SimpleContextGate&) = delete;
		SimpleContextGate& operator=(const SimpleContextGate&) = delete;

		bool Enter() override
		{
			::EnterCriticalSection(&m_lock);
			::InterlockedIncrement64(&m_enterCount);

			// 재귀 획득 감지.
			// 실제 게이트(D3D11ImmediateContextGate)는 SRWLOCK Exclusive 기반이라
			// 재귀 획득 시 데드락한다. CRITICAL_SECTION 은 재귀를 허용하므로
			// 여기서 감지해 테스트가 데드락 대신 실패로 드러나게 한다.
			if (::InterlockedIncrement(&m_depth) > 1)
				::InterlockedIncrement64(&m_recursiveEnterCount);

			return true;
		}

		void Leave() override
		{
			::InterlockedDecrement(&m_depth);
			::LeaveCriticalSection(&m_lock);
		}

		// 0 이 아니면 재귀 획득이 발생했다는 뜻이다(실제 게이트에서는 데드락).
		uint64_t GetRecursiveEnterCount() const
		{
			return static_cast<uint64_t>(
				::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_recursiveEnterCount), 0, 0));
		}

		bool IsEnabled() const override { return true; }

		// 프레임당 게이트 획득 횟수를 보기 위한 계측.
		uint64_t GetEnterCount() const
		{
			return static_cast<uint64_t>(
				::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_enterCount), 0, 0));
		}

		void ResetEnterCount()
		{
			::InterlockedExchange64(&m_enterCount, 0);
			::InterlockedExchange64(&m_recursiveEnterCount, 0);
		}

	private:
		CRITICAL_SECTION m_lock = {};
		volatile LONG64 m_enterCount = 0;
		volatile LONG64 m_recursiveEnterCount = 0;
		volatile LONG m_depth = 0;
	};
}
