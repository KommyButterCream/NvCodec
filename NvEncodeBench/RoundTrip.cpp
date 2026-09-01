#include "RoundTrip.h"
#include "GateContender.h"
#include "SimpleContextGate.h"

#include <d3d11.h>
#include <stdio.h>

#include <deque>
#include <memory>
#include <vector>

#include "../NvDecode/D3D11NvDecoder.h"
#include "../NvDecode/DecodeFrameQueue.h"
#include "../NvDecode/DecodeThread.h"
#include "../NvEncode/D3D11NvEncoder.h"
#include "../NvEncode/EncodeFrameQueue.h"
#include "../NvEncode/EncodeThread.h"

namespace Bench
{
	namespace
	{
		constexpr uint32_t kLatencyRingSize = 1024U;
		constexpr uint32_t kLatencyRingMask = kLatencyRingSize - 1U;

		// 디코드 큐 슬롯 하나가 담을 수 있는 최대 비트스트림 크기.
		// IDR 은 P 프레임의 몇 배라 넉넉히 잡는다.
		constexpr size_t kDecodeSlotBytes = 1024U * 1024U;
		constexpr size_t kDecodeQueueSlots = 8U;

		template <typename T>
		void SafeReleaseLocal(T*& instance)
		{
			if (instance)
			{
				instance->Release();
				instance = nullptr;
			}
		}

		void FillPattern(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, uint32_t patternIndex)
		{
			const uint32_t boxSize = (width < height ? width : height) / 4U;
			const uint32_t boxX = (patternIndex * 37U) % (width - boxSize);
			const uint32_t boxY = (patternIndex * 53U) % (height - boxSize);
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

					if (x >= boxX && x < boxX + boxSize && y >= boxY && y < boxY + boxSize)
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
					return;

				if (m_timer)
				{
					LARGE_INTEGER dueTime = {};
					dueTime.QuadPart = -static_cast<LONGLONG>(
						(static_cast<double>(remaining) * 10000000.0) / QpcTicksPerSecond());
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
	}

	namespace
	{
		// 앱이 디코딩된 프레임을 붙잡고 있는 상황을 재현하는 드레인 스레드.
		//
		// DecodeThread 는 콜백이 반환하는 즉시 ReleaseFrame 을 부른다. 그건
		// "콜백 안에서만 쓴다" 는 가장 흔한 사용법이고 이미 기본 경로로 검증된다.
		// 여기서는 반대쪽, 앱이 여러 장을 동시에 들고 있다가 나중에 돌려주는
		// 사용법을 검증한다. 슬롯 수보다 많이 들고 있으면 풀이 고갈되어야 하고,
		// 그때 디코더는 죽지 않고 프레임만 버려야 한다.
		class HoldingDrainThread
		{
		public:
			HoldingDrainThread(
				D3D11NvDecoder* decoder,
				DecodeFrameQueue* queue,
				uint32_t holdFrameCount,
				bool leakFrames,
				void (*onFrame)(const D3D11NvDecoder::Frame&, void*),
				void* userData)
				: m_decoder(decoder)
				, m_queue(queue)
				, m_holdFrameCount(holdFrameCount < 1U ? 1U : holdFrameCount)
				, m_leakFrames(leakFrames)
				, m_onFrame(onFrame)
				, m_userData(userData)
			{
				if (!m_decoder || !m_queue)
					return;
	
				m_thread = reinterpret_cast<HANDLE>(
					::_beginthreadex(nullptr, 0, &HoldingDrainThread::ThreadProc, this, 0, nullptr));
			}
	
			~HoldingDrainThread() { Shutdown(); }
	
			HoldingDrainThread(const HoldingDrainThread&) = delete;
			HoldingDrainThread& operator=(const HoldingDrainThread&) = delete;
	
			bool IsRunning() const { return m_thread != nullptr; }
	
			void Shutdown()
			{
				if (m_queue)
					m_queue->Shutdown();
	
				if (m_thread)
				{
					::WaitForSingleObject(m_thread, INFINITE);
					::CloseHandle(m_thread);
					m_thread = nullptr;
				}
	
				// 일부러 흘린 프레임은 여기서 전부 돌려준다.
				// 그래야 Destroy 가 슬롯을 정리할 수 있다.
				for (D3D11NvDecoder::Frame* frame : m_held)
					m_decoder->ReleaseFrame(frame);
				m_held.clear();
			}
	
			uint64_t GetParsedCount() const
			{
				return static_cast<uint64_t>(
					::ReadAcquire64(&m_parsedCount));
			}
	
		private:
			static unsigned int __stdcall ThreadProc(void* arg)
			{
				static_cast<HoldingDrainThread*>(arg)->Run();
				return 0;
			}
	
			void Run()
			{
				::SetThreadDescription(::GetCurrentThread(), L"BenchHoldingDrain");
	
				for (;;)
				{
					DecodeFrameQueue::DecodeFrameItem* item = m_queue->AcquireReadFrame();
					if (!item)
						break;
	
					if (item->size > 0 &&
						m_decoder->Parse(item->data, static_cast<uint32_t>(item->size), item->timestamp))
					{
						::InterlockedIncrement64(&m_parsedCount);
	
						while (D3D11NvDecoder::Frame* frame = m_decoder->AcquireFrame())
						{
							if (m_onFrame)
								m_onFrame(*frame, m_userData);
	
							m_held.push_back(frame);
	
							// 정해진 장수를 넘기면 가장 오래된 것부터 돌려준다.
							// leakFrames 면 영원히 돌려주지 않는다.
							if (!m_leakFrames && m_held.size() > m_holdFrameCount)
							{
								m_decoder->ReleaseFrame(m_held.front());
								m_held.pop_front();
							}
						}
					}
	
					m_queue->ReleaseReadFrame();
				}
			}
	
		private:
			D3D11NvDecoder* m_decoder = nullptr;
			DecodeFrameQueue* m_queue = nullptr;
			uint32_t m_holdFrameCount = 1;
			bool m_leakFrames = false;
			void (*m_onFrame)(const D3D11NvDecoder::Frame&, void*) = nullptr;
			void* m_userData = nullptr;
	
			std::deque<D3D11NvDecoder::Frame*> m_held;
			HANDLE m_thread = nullptr;
			volatile LONG64 m_parsedCount = 0;
		};
	}

	struct RoundTripBench::Impl
	{
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		SimpleContextGate gate;

		std::vector<ID3D11Texture2D*> patternTextures;
		std::vector<LONG> slotBusy;
		uint32_t width = 0;
		uint32_t height = 0;

		const RoundTripConfig* config = nullptr;
		RoundTripResult* result = nullptr;

		DecodeFrameQueue* decodeQueue = nullptr;
		D3D11NvDecoder* decoder = nullptr;

		struct LatencyEntry
		{
			uint64_t frameId = UINT64_MAX;
			int64_t encodeTicks = 0;
		};
		std::vector<LatencyEntry> latencyRing;
		LatencyStats latencyStats;

		CRITICAL_SECTION statsLock = {};

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

		int32_t AcquireSourceSlot()
		{
			for (size_t i = 0; i < slotBusy.size(); ++i)
			{
				if (::InterlockedCompareExchange(&slotBusy[i], 1, 0) == 0)
					return static_cast<int32_t>(i);
			}
			return -1;
		}

		void ReleaseSourceSlot(int32_t slot)
		{
			if (slot >= 0 && slot < static_cast<int32_t>(slotBusy.size()))
				::InterlockedExchange(&slotBusy[static_cast<size_t>(slot)], 0);
		}

		void RecordEncodeSubmit(uint64_t frameId)
		{
			LatencyEntry& entry = latencyRing[frameId & kLatencyRingMask];
			entry.frameId = frameId;
			entry.encodeTicks = QpcNow();
		}
	};

	// ---------- 콜백 ----------

	namespace
	{
		void OnReleaseSourceFrame(EncodeFrameQueue::InputFrameHandle& frameHandle, void* userData)
		{
			RoundTripBench::Impl* impl = static_cast<RoundTripBench::Impl*>(userData);
			if (impl)
				impl->ReleaseSourceSlot(static_cast<int32_t>(frameHandle.sourceSlotId));
		}

		// 엔코딩이 끝나면 그 비트스트림을 그대로 디코드 큐에 밀어 넣는다.
		void OnEncodedFrame(const EncodeThread::EncodedFrame& frame, void* userData)
		{
			RoundTripBench::Impl* impl = static_cast<RoundTripBench::Impl*>(userData);
			if (!impl || !impl->decodeQueue)
				return;

			::EnterCriticalSection(&impl->statsLock);
			if (impl->result)
			{
				impl->result->encodedPackets++;
				impl->result->encodedBytes += frame.size;
			}
			::LeaveCriticalSection(&impl->statsLock);

			DecodeFrameQueue::InputFrameHandle handle = {};
			handle.data = frame.data;
			handle.size = frame.size;
			handle.frameId = frame.frameId;

			// 디코더 timestamp 에는 앱이 준 frameId 를 싣는다.
			// NVDEC 를 그대로 통과해 Frame::timestamp 로 돌아오므로, 디코딩된
			// 프레임이 어느 원본 프레임인지 알 수 있는 유일한 끈이다.
			// frame.timestamp 는 엔코더 내부 카운터라 앱 프레임 번호와 무관하다.
			handle.timestamp = frame.frameId;
			handle.frameType = frame.frameType;

			// EnqueueFrame 은 데이터를 자기 버퍼로 복사한다.
			// 그래서 packet.data 가 콜백 반환 뒤에 재사용돼도 안전하다.
			if (!impl->decodeQueue->EnqueueFrame(handle))
			{
				::EnterCriticalSection(&impl->statsLock);
				if (impl->result)
					impl->result->decodeQueueRejected++;
				::LeaveCriticalSection(&impl->statsLock);
			}
		}

		void OnDecodedFrame(const D3D11NvDecoder::Frame& frame, void* userData)
		{
			RoundTripBench::Impl* impl = static_cast<RoundTripBench::Impl*>(userData);
			if (!impl)
				return;

			const int64_t now = QpcNow();

			::EnterCriticalSection(&impl->statsLock);
			if (impl->result)
			{
				impl->result->decodedFrames++;

				if (impl->result->decodedWidth == 0 && frame.texture)
				{
					D3D11_TEXTURE2D_DESC desc = {};
					frame.texture->GetDesc(&desc);
					impl->result->decodedWidth = desc.Width;
					impl->result->decodedHeight = desc.Height;
				}
			}

			// 엔코더가 넣은 timestamp 가 디코더까지 살아 돌아온다.
			// 그것으로 왕복 지연을 잰다.
			const RoundTripBench::Impl::LatencyEntry& entry =
				impl->latencyRing[frame.timestamp & kLatencyRingMask];
			if (entry.frameId == frame.timestamp)
			{
				impl->latencyStats.Add(TicksToMilliseconds(now - entry.encodeTicks));
			}
			::LeaveCriticalSection(&impl->statsLock);
		}

		void OnDecoderError(NvDecErrorCode errorCode, void* userData)
		{
			RoundTripBench::Impl* impl = static_cast<RoundTripBench::Impl*>(userData);
			if (!impl)
				return;

			const size_t index = static_cast<size_t>(errorCode);
			::EnterCriticalSection(&impl->statsLock);
			if (impl->result && index < _countof(impl->result->decoderErrorCounts))
				impl->result->decoderErrorCounts[index]++;
			::LeaveCriticalSection(&impl->statsLock);
		}
	}

	// ---------- 본체 ----------

	RoundTripBench::RoundTripBench()
		: m_impl(new Impl())
	{
	}

	RoundTripBench::~RoundTripBench()
	{
		Teardown();
		delete m_impl;
		m_impl = nullptr;
	}

	ID3D11Device* RoundTripBench::GetDevice() const
	{
		return m_impl ? m_impl->device : nullptr;
	}

	bool RoundTripBench::Setup(uint32_t width, uint32_t height)
	{
		Teardown();

		m_impl->width = width;
		m_impl->height = height;

		const D3D_FEATURE_LEVEL requested[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

		HRESULT hr = ::D3D11CreateDevice(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			requested, _countof(requested), D3D11_SDK_VERSION,
			&m_impl->device, &obtained, &m_impl->context);

		if (FAILED(hr))
		{
			printf_s("[ROUNDTRIP ERROR] D3D11CreateDevice failed. hr=0x%08lX\n",
				static_cast<unsigned long>(hr));
			return false;
		}

		constexpr uint32_t kPatternCount = 8;
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4U);
		m_impl->patternTextures.assign(kPatternCount, nullptr);
		m_impl->slotBusy.assign(kPatternCount, 0);

		for (uint32_t i = 0; i < kPatternCount; ++i)
		{
			FillPattern(pixels, width, height, i);

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

			hr = m_impl->device->CreateTexture2D(&desc, &initialData, &m_impl->patternTextures[i]);
			if (FAILED(hr))
			{
				Teardown();
				return false;
			}
		}

		return true;
	}

	void RoundTripBench::Teardown()
	{
		if (!m_impl)
			return;

		for (ID3D11Texture2D*& texture : m_impl->patternTextures)
			SafeReleaseLocal(texture);

		m_impl->patternTextures.clear();
		m_impl->slotBusy.clear();

		SafeReleaseLocal(m_impl->context);
		SafeReleaseLocal(m_impl->device);
	}

	bool RoundTripBench::Run(const RoundTripConfig& config, RoundTripResult& result)
	{
		result = RoundTripResult();

		if (!m_impl->device || m_impl->patternTextures.empty())
		{
			printf_s("[ROUNDTRIP ERROR] Setup() must succeed before Run().\n");
			return false;
		}

		m_impl->config = &config;
		m_impl->result = &result;
		m_impl->latencyStats = LatencyStats();
		m_impl->latencyStats.Reserve(config.frameCount);
		m_impl->gate.ResetEnterCount();

		for (LONG& busy : m_impl->slotBusy)
			::InterlockedExchange(&busy, 0);

		for (auto& entry : m_impl->latencyRing)
			entry = Impl::LatencyEntry();

		// ---- 디코더 ----
		NvDecConfig decoderConfig;
		decoderConfig.outputSlotCount = config.decodeSlotCount;
		decoderConfig.sharedOutputTextureMode = false;

		D3D11NvDecoder decoder;
		if (!decoder.Initialize(m_impl->device, decoderConfig, &m_impl->gate))
		{
			printf_s("[ROUNDTRIP ERROR] Decoder Initialize failed.\n");
			m_impl->config = nullptr;
			m_impl->result = nullptr;
			return false;
		}
		decoder.SetErrorCallback(OnDecoderError, m_impl);
		m_impl->decoder = &decoder;

		DecodeFrameQueue decodeQueue(kDecodeSlotBytes, kDecodeQueueSlots);
		if (!decodeQueue.IsValid())
		{
			printf_s("[ROUNDTRIP ERROR] DecodeFrameQueue allocation failed.\n");
			decoder.Destroy();
			m_impl->config = nullptr;
			m_impl->result = nullptr;
			return false;
		}
		m_impl->decodeQueue = &decodeQueue;

		// 프레임 반납 방식 두 가지 중 하나를 고른다.
		//   기본        : DecodeThread. 콜백이 반환하면 곧바로 반납한다.
		//   hold / leak : HoldingDrainThread. 앱이 여러 장을 동시에 들고 있는 경우.
		const bool useHoldingDrain = (config.holdFrameCount > 1U) || config.leakFrames;

		DecodeThread decodeThread;
		std::unique_ptr<HoldingDrainThread> holdingDrain;

		if (useHoldingDrain)
		{
			holdingDrain.reset(new HoldingDrainThread(
				&decoder, &decodeQueue,
				config.holdFrameCount, config.leakFrames,
				OnDecodedFrame, m_impl));

			if (!holdingDrain->IsRunning())
			{
				printf_s("[ROUNDTRIP ERROR] HoldingDrainThread start failed.\n");
				decoder.Destroy();
				m_impl->config = nullptr;
				m_impl->result = nullptr;
				return false;
			}
		}
		else
		{
			decodeThread.SetFrameCallback(OnDecodedFrame, m_impl);
			if (!decodeThread.Initialize(&decodeQueue, &decoder))
			{
				printf_s("[ROUNDTRIP ERROR] DecodeThread Initialize failed.\n");
				decoder.Destroy();
				m_impl->config = nullptr;
				m_impl->result = nullptr;
				return false;
			}
		}

		const auto shutdownDrain = [&]()
		{
			if (holdingDrain)
				holdingDrain->Shutdown();
			else
				decodeThread.Shutdown();
		};

		// ---- 엔코더 ----
		NvEncConfig encoderConfig;
		encoderConfig.width = config.width;
		encoderConfig.height = config.height;
		encoderConfig.encodeBufferCount = config.encodeBufferCount;
		encoderConfig.averageBitrateBps = config.bitrateBps;
		encoderConfig.frameRateNumerator = (config.targetFps > 0) ? config.targetFps : 60U;
		encoderConfig.frameRateDenominator = 1;

		D3D11NvEncoder encoder;
		if (!encoder.Initialize(m_impl->device, encoderConfig, &m_impl->gate))
		{
			printf_s("[ROUNDTRIP ERROR] Encoder Initialize failed.\n");
			shutdownDrain();
			decoder.Destroy();
			m_impl->config = nullptr;
			m_impl->result = nullptr;
			return false;
		}

		EncodeFrameQueue encodeQueue;
		if (!encodeQueue.Initialize(2, OnReleaseSourceFrame, m_impl))
		{
			printf_s("[ROUNDTRIP ERROR] EncodeFrameQueue Initialize failed.\n");
			encoder.Destroy();
			shutdownDrain();
			decoder.Destroy();
			m_impl->config = nullptr;
			m_impl->result = nullptr;
			return false;
		}

		EncodeThread encodeThread;
		encodeThread.SetEncodedFrameCallback(OnEncodedFrame, m_impl);
		if (!encodeThread.Initialize(&encodeQueue, &encoder))
		{
			printf_s("[ROUNDTRIP ERROR] EncodeThread Initialize failed.\n");
			encoder.Destroy();
			shutdownDrain();
			decoder.Destroy();
			m_impl->config = nullptr;
			m_impl->result = nullptr;
			return false;
		}

		result.initialized = true;

		// ---- 루프 ----
		// 렌더 스레드가 게이트를 빼앗는 상황까지 포함해야 실사용에 가깝다.
		GateContender contender(&m_impl->gate, config.contendHz, config.contendMicroseconds);

		const int64_t startTicks = QpcNow();
		FramePacer pacer(config.targetFps);

		// 큐에 실제로 들어간 수. 무제한 모드의 백프레셔가 이 값을 기준으로 기다린다.
		uint64_t enqueuedCount = 0;

		for (uint32_t frameIndex = 0; frameIndex < config.frameCount; ++frameIndex)
		{
			pacer.WaitForNextFrame();

			// 스로틀이 없으면 앞서 넣은 프레임이 전부 인코더에 제출되고
			// 빈 슬롯이 생길 때까지 기다린다.
			//
			// 소스 풀만 보고 게이팅하면 안 된다. EnqueueLatest 는 큐가 차면
			// 오래된 프레임을 버리면서 그 소스 슬롯을 곧바로 반납하므로,
			// 풀은 영원히 비지 않는다. 그러면 300 프레임을 수 마이크로초에
			// 소진하고 실제로는 한 장만 인코딩된 채 끝난다.
			// 제출 카운트를 봐야 드롭 없는 처리량 상한이 나온다.
			if (config.targetFps == 0 && enqueuedCount > 0)
			{
				const int64_t waitDeadline = QpcNow() + static_cast<int64_t>(QpcTicksPerSecond() * 2.0);
				while (QpcNow() < waitDeadline && !encoder.IsFaulted())
				{
					NvEncStats stats = {};
					encoder.GetStats(stats);
					if (stats.submittedFrames >= enqueuedCount
						&& stats.pendingFrames < config.encodeBufferCount)
					{
						break;
					}

					::SwitchToThread();
				}
			}

			const int32_t sourceSlot = m_impl->AcquireSourceSlot();
			if (sourceSlot < 0)
				continue;

			const uint64_t frameId = static_cast<uint64_t>(frameIndex);

			EncodeFrameQueue::InputFrameHandle handle = {};
			handle.texture = m_impl->patternTextures[static_cast<size_t>(sourceSlot)];
			handle.sourceSlotId = sourceSlot;
			handle.frameId = frameId;

			m_impl->RecordEncodeSubmit(frameId);

			if (encodeQueue.EnqueueLatest(handle, frameIndex == 0))
			{
				++enqueuedCount;
			}
			else
			{
				m_impl->ReleaseSourceSlot(sourceSlot);
			}
		}

		// ---- 종료 ----
		//
		// 세 단계를 순서대로 기다린다. 하나라도 건너뛰면 뒤쪽 단계가
		// 아직 도착하지 않은 프레임을 "유실" 로 집계한다.
		//
		//   1. 엔코드 큐 -> 엔코더   (EncodeThread 가 큐를 비울 때까지)
		//   2. 엔코더 -> 콜백        (제출된 프레임이 전부 회수될 때까지)
		//   3. 디코드 큐 -> 디코더   (파싱될 때까지)
		//
		// 예전에는 2 번만 기다리고 고정 Sleep 으로 때웠다. WaitForPendingFrames
		// 는 이미 제출된 프레임만 보므로, 큐에 남아 있던 마지막 한 장은 늘
		// 잘려나갔다. 프레임 수와 무관하게 항상 정확히 1 장이 모자랐다.

		// 1. 엔코드 큐가 비고, 넣은 만큼 제출될 때까지
		{
			const int64_t deadline = QpcNow() + static_cast<int64_t>(QpcTicksPerSecond() * 5.0);
			while (QpcNow() < deadline && !encoder.IsFaulted())
			{
				NvEncStats stats = {};
				encoder.GetStats(stats);
				if (stats.submittedFrames >= enqueuedCount)
					break;

				::Sleep(1);
			}
		}

		// 2. 제출된 프레임을 전부 회수할 때까지
		encoder.WaitForPendingFrames(5000U);

		// 3. 디코더가 큐에 남은 패킷을 전부 소화할 때까지
		{
			const int64_t deadline = QpcNow() + static_cast<int64_t>(QpcTicksPerSecond() * 5.0);

			for (;;)
			{
				NvDecStats stats = {};
				decoder.GetStats(stats);

				// deliveredFrames 가 아니라 parsedPackets 을 본다.
				// 풀 고갈 등으로 버려진 프레임은 영영 도착하지 않으므로,
				// 전달 수를 기다리면 여기서 타임아웃까지 매달린다.
				const uint64_t accounted = stats.parsedPackets + decodeQueue.GetDropCount();
				if (accounted >= result.encodedPackets || decoder.IsFaulted())
					break;

				if (QpcNow() >= deadline)
				{
					printf_s("[ROUNDTRIP WARN] Decoder drain timed out."
						" parsed %llu of %llu packets.\n",
						static_cast<unsigned long long>(accounted),
						static_cast<unsigned long long>(result.encodedPackets));
					break;
				}

				::Sleep(1);
			}
		}

		encodeThread.Shutdown();
		shutdownDrain();

		result.wallSeconds = TicksToMilliseconds(QpcNow() - startTicks) / 1000.0;

		encoder.GetStats(result.encoderStats);
		decoder.GetStats(result.decoderStats);
		result.encoderFaulted = result.encoderStats.faulted;
		result.decoderFaulted = result.decoderStats.faulted;
		result.decodeQueueDropped = decodeQueue.GetDropCount();
		result.gateEnterCount = m_impl->gate.GetEnterCount();
		result.gateRecursiveEnterCount = m_impl->gate.GetRecursiveEnterCount();
		result.contendAcquireCount = contender.GetAcquireCount();
		result.contendMaxWaitMs = contender.GetMaxWaitMs();

		encoder.Destroy();
		decoder.SetErrorCallback(nullptr, nullptr);
		decoder.Destroy();

		result.latency = m_impl->latencyStats.Summarize();

		m_impl->decodeQueue = nullptr;
		m_impl->decoder = nullptr;
		m_impl->config = nullptr;
		m_impl->result = nullptr;
		return true;
	}

	void PrintRoundTripResult(const RoundTripConfig& config, const RoundTripResult& result)
	{
		printf_s("\n");
		printf_s("=========================================================\n");
		printf_s(" round trip  %ux%u  fps=%u  encBuf=%u decSlots=%u\n",
			config.width, config.height, config.targetFps,
			config.encodeBufferCount, config.decodeSlotCount);
		printf_s("=========================================================\n");
		printf_s(" wall time            : %.3f s\n", result.wallSeconds);
		printf_s(" encoded packets      : %llu (%.2f MB)\n",
			static_cast<unsigned long long>(result.encodedPackets),
			static_cast<double>(result.encodedBytes) / (1024.0 * 1024.0));
		printf_s(" decode queue rejected: %llu\n",
			static_cast<unsigned long long>(result.decodeQueueRejected));
		printf_s(" decode queue dropped : %llu (queue full, latest wins)\n",
			static_cast<unsigned long long>(result.decodeQueueDropped));
		printf_s("---------------------------------------------------------\n");
		printf_s(" encoder submitted    : %llu\n",
			static_cast<unsigned long long>(result.encoderStats.submittedFrames));
		printf_s(" encoder completed    : %llu\n",
			static_cast<unsigned long long>(result.encoderStats.completedFrames));
		printf_s(" encoder faulted      : %s\n", result.encoderFaulted ? "YES" : "no");
		printf_s("---------------------------------------------------------\n");
		printf_s(" decoder parsed       : %llu\n",
			static_cast<unsigned long long>(result.decoderStats.parsedPackets));
		printf_s(" decoder decoded      : %llu\n",
			static_cast<unsigned long long>(result.decoderStats.decodedFrames));
		printf_s(" decoder delivered    : %llu\n",
			static_cast<unsigned long long>(result.decoderStats.deliveredFrames));
		printf_s(" dropped pool/lag/disp: %llu / %llu / %llu\n",
			static_cast<unsigned long long>(result.decoderStats.droppedPoolExhausted),
			static_cast<unsigned long long>(result.decoderStats.droppedNotConsumed),
			static_cast<unsigned long long>(result.decoderStats.droppedDisplayFailed));
		printf_s(" frames held by app   : %u\n", result.decoderStats.framesHeldByApp);
		printf_s(" decoder faulted      : %s\n", result.decoderFaulted ? "YES" : "no");
		printf_s("---------------------------------------------------------\n");
		printf_s(" callback frames      : %llu\n",
			static_cast<unsigned long long>(result.decodedFrames));
		printf_s(" decoded size         : %ux%u\n", result.decodedWidth, result.decodedHeight);
		printf_s(" gate acquisitions    : %llu\n",
			static_cast<unsigned long long>(result.gateEnterCount));
		if (result.gateRecursiveEnterCount > 0)
		{
			printf_s(" gate RECURSIVE enter : %llu  <-- 실제 게이트에서는 데드락한다\n",
				static_cast<unsigned long long>(result.gateRecursiveEnterCount));
		}
		if (result.contendAcquireCount > 0)
		{
			printf_s(" contender acquires   : %llu (max wait for gate %.3f ms)\n",
				static_cast<unsigned long long>(result.contendAcquireCount), result.contendMaxWaitMs);
		}
		printf_s("---------------------------------------------------------\n");
		printf_s(" round-trip latency (encode submit -> decode callback), n=%u\n",
			result.latency.sampleCount);
		printf_s("   min %.3f  p50 %.3f  p90 %.3f  p99 %.3f  max %.3f  [ms]\n",
			result.latency.minMs, result.latency.p50Ms, result.latency.p90Ms,
			result.latency.p99Ms, result.latency.maxMs);

		bool anyError = false;
		for (size_t i = 0; i < _countof(result.decoderErrorCounts); ++i)
		{
			if (result.decoderErrorCounts[i] == 0)
				continue;

			if (!anyError)
			{
				printf_s("---------------------------------------------------------\n");
				printf_s(" decoder error callbacks\n");
				anyError = true;
			}
			printf_s("   code %-2zu : %u\n", i, result.decoderErrorCounts[i]);
		}

		printf_s("=========================================================\n");
	}
}
