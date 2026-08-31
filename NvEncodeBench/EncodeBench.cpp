#include "EncodeBench.h"
#include "SimpleContextGate.h"

#include <d3d11.h>
#include <process.h>
#include <stdio.h>

#include <utility>
#include <vector>

#include "../NvEncode/D3D11NvEncoder.h"

namespace Bench
{
	namespace
	{
		// 지연 측정용 링. in-flight 는 한 자리 수준이므로 넉넉하다.
		constexpr uint32_t kLatencyRingSize = 1024U;
		constexpr uint32_t kLatencyRingMask = kLatencyRingSize - 1U;

		template <typename T>
		void SafeReleaseLocal(T*& instance)
		{
			if (instance)
			{
				instance->Release();
				instance = nullptr;
			}
		}

		// 압축이 자명하지 않도록 그라데이션 + 이동하는 사각형 + 약한 노이즈를 섞는다.
		void FillPattern(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
			uint32_t patternIndex, uint32_t patternCount)
		{
			const double phase = static_cast<double>(patternIndex) / static_cast<double>(patternCount);
			const uint32_t boxSize = (width < height ? width : height) / 4U;
			const uint32_t boxX = static_cast<uint32_t>(phase * static_cast<double>(width - boxSize));
			const uint32_t boxY = static_cast<uint32_t>((1.0 - phase) * static_cast<double>(height - boxSize));

			uint32_t lcg = 0x9E3779B9u ^ (patternIndex * 0x85EBCA6Bu);

			for (uint32_t y = 0; y < height; ++y)
			{
				uint8_t* row = pixels.data() + static_cast<size_t>(y) * width * 4U;
				for (uint32_t x = 0; x < width; ++x)
				{
					lcg = lcg * 1664525u + 1013904223u;
					const uint8_t noise = static_cast<uint8_t>((lcg >> 24) & 0x0Fu);

					uint8_t b = static_cast<uint8_t>((x * 255U / width + patternIndex * 8U) & 0xFFU);
					uint8_t g = static_cast<uint8_t>((y * 255U / height) & 0xFFU);
					uint8_t r = static_cast<uint8_t>(((x ^ y) + patternIndex * 13U) & 0xFFU);

					const bool insideBox =
						(x >= boxX && x < boxX + boxSize && y >= boxY && y < boxY + boxSize);
					if (insideBox)
					{
						b = static_cast<uint8_t>(240U - noise);
						g = static_cast<uint8_t>(200U - noise);
						r = static_cast<uint8_t>(64U + noise);
					}

					uint8_t* pixel = row + static_cast<size_t>(x) * 4U;
					pixel[0] = static_cast<uint8_t>(b ^ noise);
					pixel[1] = static_cast<uint8_t>(g ^ noise);
					pixel[2] = static_cast<uint8_t>(r ^ noise);
					pixel[3] = 255U;
				}
			}
		}

		// 목표 fps 로 프레임 투입 시각을 맞춘다.
		class FramePacer
		{
		public:
			explicit FramePacer(uint32_t targetFps)
				: m_enabled(targetFps > 0)
			{
				if (!m_enabled)
					return;

				m_intervalTicks = static_cast<int64_t>(QpcTicksPerSecond() / static_cast<double>(targetFps));
				m_nextDeadline = QpcNow();

				m_timer = ::CreateWaitableTimerExW(
					nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
				if (!m_timer)
				{
					// 고해상도 타이머를 못 만들면 일반 타이머로 내려간다.
					m_timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
				}
			}

			~FramePacer()
			{
				if (m_timer)
					::CloseHandle(m_timer);
			}

			FramePacer(const FramePacer&) = delete;
			FramePacer& operator=(const FramePacer&) = delete;

			void WaitForNextFrame()
			{
				if (!m_enabled)
					return;

				m_nextDeadline += m_intervalTicks;

				const int64_t remaining = m_nextDeadline - QpcNow();
				if (remaining <= 0)
					return;   // 이미 늦었다. 따라잡지 않고 그냥 진행한다.

				if (m_timer)
				{
					LARGE_INTEGER dueTime = {};
					// 음수는 상대 시간(100ns 단위).
					const double hundredNanos =
						(static_cast<double>(remaining) * 10000000.0) / QpcTicksPerSecond();
					dueTime.QuadPart = -static_cast<LONGLONG>(hundredNanos);
					if (::SetWaitableTimer(m_timer, &dueTime, 0, nullptr, nullptr, FALSE))
					{
						::WaitForSingleObject(m_timer, INFINITE);
						return;
					}
				}

				while (QpcNow() < m_nextDeadline)
					::Sleep(0);
			}

		private:
			bool m_enabled = false;
			int64_t m_intervalTicks = 0;
			int64_t m_nextDeadline = 0;
			HANDLE m_timer = nullptr;
		};

		// 게이트를 주기적으로 점유하는 가짜 렌더 스레드.
		// 실제 렌더러는 OMSetRenderTargets -> Draw -> Present 시퀀스를
		// 게이트 안에서 수행하므로, 그 점유 패턴을 흉내낸다.
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
					::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_acquireCount), 0, 0));
			}

			// 게이트를 얻기까지 기다린 최대 시간. 엔코더가 게이트를 오래 잡으면 커진다.
			double GetMaxWaitMs() const
			{
				const LONG64 ticks =
					::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&m_maxWaitTicks), 0, 0);
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
						if (waited > ::InterlockedCompareExchange64(&m_maxWaitTicks, 0, 0))
							::InterlockedExchange64(&m_maxWaitTicks, waited);

						::InterlockedIncrement64(&m_acquireCount);
						SpinForMicroseconds(m_holdMicroseconds);
					}

					const int64_t remaining = nextDeadline - QpcNow();
					if (remaining > 0)
					{
						const DWORD sleepMs = static_cast<DWORD>(
							TicksToMilliseconds(remaining));
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
			volatile LONG64 m_acquireCount = 0;
			volatile LONG64 m_maxWaitTicks = 0;
		};

		bool WriteCsv(const std::string& path,
			const std::vector<std::pair<uint64_t, double>>& rows,
			const std::vector<uint32_t>& sizes)
		{
			FILE* file = nullptr;
			if (::fopen_s(&file, path.c_str(), "wb") != 0 || !file)
				return false;

			::fprintf(file, "frameId,latencyMs,packetBytes\n");
			for (size_t i = 0; i < rows.size(); ++i)
			{
				const uint32_t size = (i < sizes.size()) ? sizes[i] : 0U;
				::fprintf(file, "%llu,%.4f,%u\n",
					static_cast<unsigned long long>(rows[i].first), rows[i].second, size);
			}

			::fclose(file);
			return true;
		}
	}

	const char* ToString(NvEncErrorCode errorCode)
	{
		switch (errorCode)
		{
		case NvEncErrorCode::None:              return "None";
		case NvEncErrorCode::OutputReadFailed:  return "OutputReadFailed";
		case NvEncErrorCode::OutputTimeout:     return "OutputTimeout";
		case NvEncErrorCode::OutputUnmapFailed: return "OutputUnmapFailed";
		case NvEncErrorCode::RingCorrupted:     return "RingCorrupted";
		case NvEncErrorCode::EncoderFaulted:    return "EncoderFaulted";
		default:                                return "Unknown";
		}
	}

	struct EncodeBench::Impl
	{
		// 생산자 텍스처 풀
		std::vector<ID3D11Texture2D*> patternTextures;
		std::vector<LONG> slotBusy;               // 0 = free, 1 = in use
		uint32_t width = 0;
		uint32_t height = 0;

		SimpleContextGate gate;

		// 실행 중에만 유효한 상태
		const BenchConfig* config = nullptr;
		BenchResult* result = nullptr;

		struct LatencyEntry
		{
			uint64_t frameId = UINT64_MAX;
			int64_t enqueueTicks = 0;
		};
		std::vector<LatencyEntry> latencyRing;
		LatencyStats latencyStats;
		CRITICAL_SECTION statsLock = {};

		std::vector<std::pair<uint64_t, double>> csvRows;   // frameId, latencyMs
		std::vector<uint32_t> csvSizes;

		Impl()
		{
			::InitializeCriticalSectionAndSpinCount(&statsLock, 2000);
			latencyRing.resize(kLatencyRingSize);
		}

		~Impl()
		{
			::DeleteCriticalSection(&statsLock);
		}

		Impl(const Impl&) = delete;
		Impl& operator=(const Impl&) = delete;

		void ResetRun()
		{
			for (LatencyEntry& entry : latencyRing)
				entry = LatencyEntry();

			latencyStats = LatencyStats();
			csvRows.clear();
			csvSizes.clear();

			for (LONG& busy : slotBusy)
				::InterlockedExchange(&busy, 0);
		}

		int32_t AcquireSlot()
		{
			for (size_t i = 0; i < slotBusy.size(); ++i)
			{
				if (::InterlockedCompareExchange(&slotBusy[i], 1, 0) == 0)
					return static_cast<int32_t>(i);
			}
			return -1;
		}

		void ReleaseSlot(int32_t slot)
		{
			if (slot >= 0 && slot < static_cast<int32_t>(slotBusy.size()))
				::InterlockedExchange(&slotBusy[static_cast<size_t>(slot)], 0);
		}

		void RecordEnqueue(uint64_t frameId)
		{
			LatencyEntry& entry = latencyRing[frameId & kLatencyRingMask];
			entry.frameId = frameId;
			entry.enqueueTicks = QpcNow();
		}

		void RecordPacket(uint64_t frameId, uint32_t size, bool isKeyFrame)
		{
			const int64_t now = QpcNow();

			::EnterCriticalSection(&statsLock);

			if (result)
			{
				result->encodedPackets++;
				result->totalBytes += size;
				if (isKeyFrame)
					result->keyFrames++;
			}

			const LatencyEntry& entry = latencyRing[frameId & kLatencyRingMask];
			if (entry.frameId == frameId)
			{
				const double latencyMs = TicksToMilliseconds(now - entry.enqueueTicks);
				latencyStats.Add(latencyMs);
				if (config && !config->csvPath.empty())
				{
					csvRows.emplace_back(frameId, latencyMs);
					csvSizes.push_back(size);
				}
			}
			else if (result)
			{
				result->latencyUnmatched++;
			}

			::LeaveCriticalSection(&statsLock);
		}

		void SpinCallbackDelay() const
		{
			if (config)
				SpinForMicroseconds(config->callbackDelayMicroseconds);
		}

		void RecordError(NvEncErrorCode errorCode)
		{
			const size_t index = static_cast<size_t>(errorCode);

			::EnterCriticalSection(&statsLock);
			if (result && index < _countof(result->errorCounts))
				result->errorCounts[index]++;
			::LeaveCriticalSection(&statsLock);
		}
	};

	void EncodeBench::OnReleaseFrame(EncodeFrameQueue::InputFrameHandle& frameHandle, void* userData)
	{
		Impl* impl = static_cast<Impl*>(userData);
		if (impl)
			impl->ReleaseSlot(static_cast<int32_t>(frameHandle.sourceSlotId));
	}

	void EncodeBench::OnEncodedFrame(const EncodeThread::EncodedFrame& frame, void* userData)
	{
		Impl* impl = static_cast<Impl*>(userData);
		if (!impl)
			return;

		// 시각을 먼저 찍는다. 그래야 측정된 지연이 인위적 콜백 지연으로
		// 자동 부풀려지지 않고, 파이프라인이 패킷을 전달한 시점을 뜻한다.
		impl->RecordPacket(frame.frameId, frame.size, frame.isKeyFrame);
		impl->SpinCallbackDelay();
	}

	void EncodeBench::OnEncoderError(NvEncErrorCode errorCode, void* userData)
	{
		Impl* impl = static_cast<Impl*>(userData);
		if (impl)
			impl->RecordError(errorCode);
	}

	EncodeBench::EncodeBench()
		: m_impl(new Impl())
	{
	}

	EncodeBench::~EncodeBench()
	{
		Teardown();
		delete m_impl;
		m_impl = nullptr;
	}

	bool EncodeBench::Setup(uint32_t width, uint32_t height, uint32_t sourcePoolCount)
	{
		Teardown();

		if (width == 0 || height == 0 || sourcePoolCount == 0)
			return false;

		m_impl->width = width;
		m_impl->height = height;

		const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		const D3D_FEATURE_LEVEL requested[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

		HRESULT hr = ::D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			flags,
			requested,
			_countof(requested),
			D3D11_SDK_VERSION,
			&m_device,
			&obtained,
			&m_context);

		if (FAILED(hr))
		{
			printf_s("[BENCH ERROR] D3D11CreateDevice failed. hr=0x%08lX\n",
				static_cast<unsigned long>(hr));
			return false;
		}

		// 패턴 텍스처를 미리 만들어 두고 매 프레임 돌려 쓴다.
		// 프레임마다 CPU 로 채우면 CPU 시간이 측정값을 지배한다.
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4U);
		m_impl->patternTextures.assign(sourcePoolCount, nullptr);
		m_impl->slotBusy.assign(sourcePoolCount, 0);

		for (uint32_t i = 0; i < sourcePoolCount; ++i)
		{
			FillPattern(pixels, width, height, i, sourcePoolCount);

			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			D3D11_SUBRESOURCE_DATA initialData = {};
			initialData.pSysMem = pixels.data();
			initialData.SysMemPitch = width * 4U;

			hr = m_device->CreateTexture2D(&desc, &initialData, &m_impl->patternTextures[i]);
			if (FAILED(hr))
			{
				printf_s("[BENCH ERROR] CreateTexture2D failed. index=%u hr=0x%08lX\n",
					i, static_cast<unsigned long>(hr));
				Teardown();
				return false;
			}
		}

		return true;
	}

	void EncodeBench::Teardown()
	{
		if (m_impl)
		{
			for (ID3D11Texture2D*& texture : m_impl->patternTextures)
				SafeReleaseLocal(texture);

			m_impl->patternTextures.clear();
			m_impl->slotBusy.clear();
		}

		SafeReleaseLocal(m_context);
		SafeReleaseLocal(m_device);
	}

	bool EncodeBench::Run(const BenchConfig& config, BenchResult& result)
	{
		result = BenchResult();

		if (!m_device || m_impl->patternTextures.empty())
		{
			printf_s("[BENCH ERROR] Setup() must succeed before Run().\n");
			return false;
		}

		if (config.width != m_impl->width || config.height != m_impl->height)
		{
			printf_s("[BENCH ERROR] Run() resolution must match Setup(). setup=%ux%u run=%ux%u\n",
				m_impl->width, m_impl->height, config.width, config.height);
			return false;
		}

		m_impl->ResetRun();
		m_impl->config = &config;
		m_impl->result = &result;
		m_impl->gate.ResetEnterCount();
		m_impl->latencyStats.Reserve(config.frameCount);

		D3D11NvEncoder encoder;
		const uint64_t gateCountBeforeInit = m_impl->gate.GetEnterCount();
		if (!encoder.Initialize(
			m_device,
			config.width,
			config.height,
			config.encodeBufferCount,
			&m_impl->gate,
			config.asyncPipeline))
		{
			printf_s("[BENCH ERROR] Encoder Initialize failed. encodeBufferCount=%u\n",
				config.encodeBufferCount);
			m_impl->config = nullptr;
			m_impl->result = nullptr;
			return false;
		}

		result.gateEnterCountDuringInit = m_impl->gate.GetEnterCount() - gateCountBeforeInit;

		encoder.SetErrorCallback(OnEncoderError, m_impl);
		result.initialized = true;

		// 가짜 렌더 스레드. 게이트만으로 보호하는 구성의 실제 위험을 재현한다.
		GateContender contender(&m_impl->gate, config.contendHz, config.contendMicroseconds);

		const int64_t startTicks = QpcNow();

		const bool ok = config.asyncPipeline
			? RunAsync(config, result, encoder)
			: RunSync(config, result, encoder);

		result.wallSeconds = TicksToMilliseconds(QpcNow() - startTicks) / 1000.0;

		encoder.GetStats(result.encoderStats);
		result.faultedAtEnd = result.encoderStats.faulted;
		result.gateEnterCount = m_impl->gate.GetEnterCount();

		const uint64_t gateCountBeforeDestroy = m_impl->gate.GetEnterCount();
		encoder.Destroy();
		result.gateEnterCountDuringDestroy = m_impl->gate.GetEnterCount() - gateCountBeforeDestroy;
		result.gateRecursiveEnterCount = m_impl->gate.GetRecursiveEnterCount();
		result.contendAcquireCount = contender.GetAcquireCount();
		result.contendMaxWaitMs = contender.GetMaxWaitMs();

		encoder.SetErrorCallback(nullptr, nullptr);

		result.latency = m_impl->latencyStats.Summarize();

		if (!config.csvPath.empty())
		{
			if (WriteCsv(config.csvPath, m_impl->csvRows, m_impl->csvSizes))
			{
				printf_s("CSV written: %s (%zu rows)\n", config.csvPath.c_str(), m_impl->csvRows.size());
			}
			else
			{
				printf_s("[BENCH ERROR] Failed to write CSV: %s\n", config.csvPath.c_str());
			}
		}

		m_impl->config = nullptr;
		m_impl->result = nullptr;
		return ok;
	}

	bool EncodeBench::RunAsync(const BenchConfig& config, BenchResult& result, D3D11NvEncoder& encoder)
	{
		EncodeFrameQueue queue;
		if (!queue.Initialize(config.queueFrameCount, OnReleaseFrame, m_impl))
		{
			printf_s("[BENCH ERROR] Queue Initialize failed. frameCount=%u\n", config.queueFrameCount);
			return false;
		}

		EncodeThread encodeThread;
		// 콜백은 스레드를 띄우기 전에 설정한다.
		encodeThread.SetEncodedFrameCallback(OnEncodedFrame, m_impl);
		if (!encodeThread.Initialize(&queue, &encoder))
		{
			printf_s("[BENCH ERROR] EncodeThread Initialize failed.\n");
			return false;
		}

		FramePacer pacer(config.targetFps);

		for (uint32_t frameIndex = 0; frameIndex < config.frameCount; ++frameIndex)
		{
			pacer.WaitForNextFrame();

			// 스로틀이 없으면 앞서 넣은 프레임이 모두 인코더에 제출되고
			// 빈 슬롯이 생길 때까지 기다린다.
			//
			// 큐 소비 카운트로는 게이팅할 수 없다. EncodeThread 는 인코더
			// 슬롯이 없으면 프레임을 버리면서도 ReleaseReadFrame 으로
			// "처리됨"으로 세기 때문에, 큐 기준으로는 전부 소비된 것처럼 보인다.
			// 제출 카운트를 봐야 실제로 인코딩된 프레임만 남는다.
			if (config.targetFps == 0 && result.enqueued > 0)
			{
				const int64_t waitDeadline = QpcNow() + static_cast<int64_t>(QpcTicksPerSecond() * 2.0);
				while (QpcNow() < waitDeadline && !encoder.IsFaulted())
				{
					NvEncStats stats = {};
					encoder.GetStats(stats);
					if (stats.submittedFrames >= result.enqueued
						&& stats.pendingFrames < config.encodeBufferCount)
					{
						break;
					}

					::SwitchToThread();
				}
			}

			const int32_t slot = m_impl->AcquireSlot();
			if (slot < 0)
			{
				// 소스 풀이 비었다. 실제 캡처 엔진도 이 경우 프레임을 버린다.
				result.producerStalled++;
				continue;
			}

			if (config.faultInjectCount > 0 && frameIndex == config.faultInjectAfterFrames)
			{
				printf_s("  >> injecting %u output failures at frame %u\n",
					config.faultInjectCount, frameIndex);
				encoder.DebugFailNextOutputs(config.faultInjectCount);
			}

			const uint64_t frameId = static_cast<uint64_t>(frameIndex);
			const bool forceKeyFrame =
				(config.keyFrameInterval > 0) && (frameIndex % config.keyFrameInterval == 0);

			EncodeFrameQueue::InputFrameHandle handle = {};
			handle.texture = m_impl->patternTextures[static_cast<size_t>(slot)];
			handle.sourceSlotId = slot;
			handle.frameId = frameId;

			m_impl->RecordEnqueue(frameId);

			if (queue.EnqueueLatest(handle, forceKeyFrame))
			{
				result.enqueued++;
			}
			else
			{
				result.enqueueRejected++;
				m_impl->ReleaseSlot(slot);
			}
		}

		// 큐에 남은 프레임까지 전부 흘려보낸다.
		// WaitForPendingFrames 만으로는 부족하다. 큐에 아직 프레임이 있고
		// 인코더에 제출되지 않은 상태면 pending 이 0 이라 즉시 통과해버린다.
		{
			const int64_t drainDeadline = QpcNow() + static_cast<int64_t>(QpcTicksPerSecond() * 5.0);
			uint32_t previousProcessCount = UINT32_MAX;
			while (QpcNow() < drainDeadline && !encoder.IsFaulted())
			{
				const uint32_t processCount = queue.GetProcessCount();
				if (encoder.GetPendingFrameCount() == 0 && processCount == previousProcessCount)
					break;   // 인코더가 비었고 큐도 더 움직이지 않는다.

				previousProcessCount = processCount;
				::Sleep(2);
			}
		}

		encoder.WaitForPendingFrames(5000U);

		encodeThread.GetStats(result.threadStats);
		encodeThread.Shutdown();

		result.queueDropCount = queue.GetDropCount();
		result.queueProcessCount = queue.GetProcessCount();
		return true;
	}

	bool EncodeBench::RunSync(const BenchConfig& config, BenchResult& result, D3D11NvEncoder& encoder)
	{
		// 동기 경로는 스레드 홉이 없어 순수 인코딩 지연의 하한을 본다.
		FramePacer pacer(config.targetFps);

		for (uint32_t frameIndex = 0; frameIndex < config.frameCount; ++frameIndex)
		{
			pacer.WaitForNextFrame();

			const uint64_t frameId = static_cast<uint64_t>(frameIndex);
			ID3D11Texture2D* source =
				m_impl->patternTextures[frameIndex % m_impl->patternTextures.size()];

			if (config.faultInjectCount > 0 && frameIndex == config.faultInjectAfterFrames)
			{
				printf_s("  >> injecting %u output failures at frame %u\n",
					config.faultInjectCount, frameIndex);
				encoder.DebugFailNextOutputs(config.faultInjectCount);
			}

			if ((config.keyFrameInterval > 0) && (frameIndex % config.keyFrameInterval == 0))
				encoder.RequestKeyFrame();

			m_impl->RecordEnqueue(frameId);

			if (!encoder.PrepareFrameForEncode(source))
			{
				result.enqueueRejected++;
				continue;
			}

			result.enqueued++;

			NvEncPacket packet = {};
			if (encoder.DoEncode(packet))
			{
				m_impl->RecordPacket(frameId, packet.size, packet.isKeyFrame);
				m_impl->SpinCallbackDelay();
			}
		}

		return true;
	}

	void PrintResult(const BenchConfig& config, const BenchResult& result)
	{
		const double fps = (result.wallSeconds > 0.0)
			? static_cast<double>(result.encodedPackets) / result.wallSeconds
			: 0.0;
		const double mbps = (result.wallSeconds > 0.0)
			? (static_cast<double>(result.totalBytes) * 8.0) / (result.wallSeconds * 1000000.0)
			: 0.0;
		const double avgPacketKb = (result.encodedPackets > 0)
			? static_cast<double>(result.totalBytes) / static_cast<double>(result.encodedPackets) / 1024.0
			: 0.0;
		const double gatePerFrame = (result.encoderStats.submittedFrames > 0)
			? static_cast<double>(result.gateEnterCount) / static_cast<double>(result.encoderStats.submittedFrames)
			: 0.0;

		printf_s("\n");
		printf_s("=========================================================\n");
		printf_s(" %ux%u  %s  buffers=%u queue=%u targetFps=%u\n",
			config.width, config.height,
			config.asyncPipeline ? "async" : "sync",
			config.encodeBufferCount, config.queueFrameCount, config.targetFps);
		if (config.callbackDelayMicroseconds > 0)
		{
			printf_s(" callback delay       : %u us (spin)\n", config.callbackDelayMicroseconds);
		}
		printf_s("=========================================================\n");
		printf_s(" wall time            : %.3f s\n", result.wallSeconds);
		printf_s(" enqueued / rejected  : %llu / %llu\n",
			static_cast<unsigned long long>(result.enqueued),
			static_cast<unsigned long long>(result.enqueueRejected));
		printf_s(" producer stalled     : %llu\n",
			static_cast<unsigned long long>(result.producerStalled));
		printf_s(" queue dropped / proc : %u / %u\n",
			result.queueDropCount, result.queueProcessCount);

		// EncodeThread 가 큐에서 꺼냈지만 인코더에 넣지 못한 프레임.
		// 큐의 dropCount 에는 잡히지 않아 EncodeThread 가 직접 센다.
		if (config.asyncPipeline)
		{
			printf_s(" enc thread submitted : %llu\n",
				static_cast<unsigned long long>(result.threadStats.framesSubmitted));
			printf_s(" dropped, no enc slot : %llu\n",
				static_cast<unsigned long long>(result.threadStats.framesDroppedNoEncoderSlot));
			if (result.threadStats.framesDroppedPrepareFailed > 0
				|| result.threadStats.framesDroppedSubmitFailed > 0)
			{
				printf_s(" dropped, prep/submit : %llu / %llu\n",
					static_cast<unsigned long long>(result.threadStats.framesDroppedPrepareFailed),
					static_cast<unsigned long long>(result.threadStats.framesDroppedSubmitFailed));
			}
		}

		printf_s("---------------------------------------------------------\n");
		printf_s(" encoder submitted    : %llu\n",
			static_cast<unsigned long long>(result.encoderStats.submittedFrames));
		printf_s(" encoder completed    : %llu\n",
			static_cast<unsigned long long>(result.encoderStats.completedFrames));
		printf_s(" encoder lost         : %llu\n",
			static_cast<unsigned long long>(result.encoderStats.lostFrames));
		printf_s(" encoder pending      : %u\n", result.encoderStats.pendingFrames);
		printf_s(" faulted              : %s\n", result.faultedAtEnd ? "YES" : "no");
		printf_s("---------------------------------------------------------\n");
		printf_s(" packets received     : %llu (keyframes %llu)\n",
			static_cast<unsigned long long>(result.encodedPackets),
			static_cast<unsigned long long>(result.keyFrames));
		printf_s(" throughput           : %.2f fps\n", fps);
		printf_s(" bitrate              : %.2f Mbps  (avg packet %.1f KB)\n", mbps, avgPacketKb);
		printf_s(" gate acquisitions    : %llu (%.1f per submitted frame)\n",
			static_cast<unsigned long long>(result.gateEnterCount), gatePerFrame);
		printf_s(" gate init / destroy  : %llu / %llu\n",
			static_cast<unsigned long long>(result.gateEnterCountDuringInit),
			static_cast<unsigned long long>(result.gateEnterCountDuringDestroy));
		if (result.contendAcquireCount > 0)
		{
			printf_s(" contender acquires   : %llu (max wait for gate %.3f ms)\n",
				static_cast<unsigned long long>(result.contendAcquireCount), result.contendMaxWaitMs);
		}
		if (result.gateRecursiveEnterCount > 0)
		{
			printf_s(" gate RECURSIVE enter : %llu  <-- 실제 게이트에서는 데드락한다\n",
				static_cast<unsigned long long>(result.gateRecursiveEnterCount));
		}
		printf_s("---------------------------------------------------------\n");
		printf_s(" latency (enqueue -> packet), n=%u\n", result.latency.sampleCount);
		printf_s("   min %.3f  p50 %.3f  p90 %.3f  p99 %.3f  max %.3f  mean %.3f  [ms]\n",
			result.latency.minMs, result.latency.p50Ms, result.latency.p90Ms,
			result.latency.p99Ms, result.latency.maxMs, result.latency.meanMs);
		if (result.latencyUnmatched > 0)
		{
			printf_s("   unmatched packets  : %llu\n",
				static_cast<unsigned long long>(result.latencyUnmatched));
		}

		bool anyError = false;
		for (size_t i = 0; i < _countof(result.errorCounts); ++i)
		{
			if (result.errorCounts[i] == 0)
				continue;

			if (!anyError)
			{
				printf_s("---------------------------------------------------------\n");
				printf_s(" error callbacks\n");
				anyError = true;
			}

			printf_s("   %-18s : %u\n",
				ToString(static_cast<NvEncErrorCode>(i)), result.errorCounts[i]);
		}

		printf_s("=========================================================\n");
	}
}
