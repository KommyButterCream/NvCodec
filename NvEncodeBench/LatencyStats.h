#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <stdint.h>

#include <algorithm>
#include <vector>

namespace Bench
{
	inline int64_t QpcNow()
	{
		LARGE_INTEGER counter = {};
		::QueryPerformanceCounter(&counter);
		return counter.QuadPart;
	}

	inline double QpcTicksPerSecond()
	{
		static const double frequency = []() -> double
		{
			LARGE_INTEGER value = {};
			::QueryPerformanceFrequency(&value);
			return static_cast<double>(value.QuadPart);
		}();
		return frequency;
	}

	inline double TicksToMilliseconds(int64_t ticks)
	{
		return (static_cast<double>(ticks) * 1000.0) / QpcTicksPerSecond();
	}

	// 지정한 마이크로초 동안 CPU 를 점유한다.
	// Sleep 은 1ms 미만을 표현할 수 없고 스레드를 양보해버려서
	// CPU 바운드 콜백(브로드캐스트)을 흉내내지 못한다.
	inline void SpinForMicroseconds(uint32_t microseconds)
	{
		if (microseconds == 0)
			return;

		const int64_t targetTicks =
			static_cast<int64_t>((QpcTicksPerSecond() * static_cast<double>(microseconds)) / 1000000.0);
		const int64_t deadline = QpcNow() + targetTicks;

		while (QpcNow() < deadline)
		{
			// 하이퍼스레딩 형제 코어에 여유를 준다.
			::YieldProcessor();
		}
	}

	struct LatencySummary
	{
		uint32_t sampleCount = 0;
		double minMs = 0.0;
		double meanMs = 0.0;
		double p50Ms = 0.0;
		double p90Ms = 0.0;
		double p99Ms = 0.0;
		double maxMs = 0.0;
	};

	// 지연 샘플을 모아 백분위를 계산한다.
	// 프레임 수가 유한하므로 전부 보관한 뒤 정렬한다(근사 없이 정확).
	class LatencyStats
	{
	public:
		void Reserve(size_t count) { m_samples.reserve(count); }

		void Add(double milliseconds) { m_samples.push_back(milliseconds); }

		size_t Count() const { return m_samples.size(); }

		const std::vector<double>& Samples() const { return m_samples; }

		LatencySummary Summarize() const
		{
			LatencySummary summary = {};
			if (m_samples.empty())
				return summary;

			std::vector<double> sorted = m_samples;
			std::sort(sorted.begin(), sorted.end());

			double total = 0.0;
			for (const double value : sorted)
				total += value;

			summary.sampleCount = static_cast<uint32_t>(sorted.size());
			summary.minMs = sorted.front();
			summary.maxMs = sorted.back();
			summary.meanMs = total / static_cast<double>(sorted.size());
			summary.p50Ms = Percentile(sorted, 0.50);
			summary.p90Ms = Percentile(sorted, 0.90);
			summary.p99Ms = Percentile(sorted, 0.99);
			return summary;
		}

	private:
		static double Percentile(const std::vector<double>& sorted, double fraction)
		{
			if (sorted.empty())
				return 0.0;

			const double position = fraction * static_cast<double>(sorted.size() - 1);
			const size_t lowerIndex = static_cast<size_t>(position);
			const size_t upperIndex = (std::min)(lowerIndex + 1, sorted.size() - 1);
			const double weight = position - static_cast<double>(lowerIndex);
			return sorted[lowerIndex] * (1.0 - weight) + sorted[upperIndex] * weight;
		}

	private:
		std::vector<double> m_samples;
	};
}
