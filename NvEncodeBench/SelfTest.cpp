#include "SelfTest.h"

#include <stdio.h>

#include "EncodeBench.h"
#include "RoundTrip.h"
#include "SimpleContextGate.h"
#include "../NvDecode/D3D11NvDecoder.h"
#include "../NvEncode/D3D11NvEncoder.h"
#include "../NvEncode/EncodeFrameQueue.h"

namespace Bench
{
	namespace
	{
		uint32_t g_passed = 0;
		uint32_t g_failed = 0;

		void BeginCase(const char* name)
		{
			printf_s("\n[CASE] %s\n", name);
		}

		bool Check(bool condition, const char* what)
		{
			if (condition)
			{
				printf_s("   ok   : %s\n", what);
			}
			else
			{
				printf_s("   FAIL : %s\n", what);
			}
			return condition;
		}

		void EndCase(bool ok)
		{
			if (ok)
			{
				++g_passed;
				printf_s("  => PASS\n");
			}
			else
			{
				++g_failed;
				printf_s("  => FAIL\n");
			}
		}

		// 큐 재초기화 검증용. 반납 콜백이 몇 번 불렸는지만 센다.
		struct ReleaseCounter
		{
			uint32_t count = 0;
		};

		void CountingRelease(EncodeFrameQueue::InputFrameHandle& frameHandle, void* userData)
		{
			(void)frameHandle;
			ReleaseCounter* counter = static_cast<ReleaseCounter*>(userData);
			if (counter)
				counter->count++;
		}

		// Bug D : 잘못된 버퍼 수량은 조용히 스톨하지 않고 초기화 단계에서 거절돼야 한다.
		bool TestRejectsInvalidBufferCounts(EncodeBench& bench)
		{
			BeginCase("Bug D - invalid buffer counts are rejected up front");

			bool ok = true;
			ReleaseCounter counter;
			EncodeFrameQueue queue;

			ok &= Check(!queue.Initialize(1, CountingRelease, &counter),
				"queue rejects frameCount = 1");
			ok &= Check(!queue.Initialize(3, CountingRelease, &counter),
				"queue rejects frameCount = 3 (not a power of two)");
			ok &= Check(!queue.Initialize(4, nullptr, &counter),
				"queue rejects null release callback");
			ok &= Check(queue.Initialize(2, CountingRelease, &counter),
				"queue accepts frameCount = 2");

			D3D11NvEncoder encoder;
			SimpleContextGate gate;
			ID3D11Device* device = bench.GetDevice();

			ok &= Check(!encoder.Initialize(device, 640, 480, 1, &gate, true),
				"encoder rejects encodeBufferCount = 1");
			ok &= Check(!encoder.Initialize(device, 640, 480, 3, &gate, true),
				"encoder rejects encodeBufferCount = 3 (not a power of two)");
			encoder.Destroy();

			EndCase(ok);
			return ok;
		}

		// Bug B : Shutdown 후 다시 Initialize 할 수 있어야 한다(스트림 재시작).
		bool TestQueueCanBeReinitialized()
		{
			BeginCase("Bug B - queue can be re-initialized after shutdown");

			bool ok = true;
			ReleaseCounter counter;
			EncodeFrameQueue queue;

			ok &= Check(queue.Initialize(4, CountingRelease, &counter),
				"first Initialize(4) succeeds");

			// 처리되지 않은 프레임을 하나 넣어두고 닫는다.
			// Shutdown 이 생산자에게 반납해야 한다.
			EncodeFrameQueue::InputFrameHandle handle = {};
			handle.texture = reinterpret_cast<ID3D11Texture2D*>(0x1);   // 반납 여부만 확인
			handle.sourceSlotId = 0;
			handle.frameId = 0;
			ok &= Check(queue.EnqueueLatest(handle, false), "EnqueueLatest succeeds");

			queue.Shutdown();
			ok &= Check(counter.count == 1, "Shutdown returns the queued frame to the producer");

			ok &= Check(queue.Initialize(4, CountingRelease, &counter),
				"re-Initialize with the same frameCount succeeds");
			ok &= Check(queue.Initialize(8, CountingRelease, &counter),
				"re-Initialize with a different frameCount succeeds");

			// 재초기화 후에도 실제로 동작해야 한다.
			ok &= Check(queue.EnqueueLatest(handle, false),
				"queue still accepts frames after re-Initialize");

			EndCase(ok);
			return ok;
		}

		// Bug C : 종료 시 in-flight 프레임이 전부 드레인되고 결과가 유실되지 않아야 한다.
		bool TestCleanShutdownDrainsEveryFrame(EncodeBench& bench)
		{
			BeginCase("Bug C - clean shutdown drains every in-flight frame");

			BenchConfig config;
			config.width = 1280;
			config.height = 720;
			config.frameCount = 180;
			config.targetFps = 120;
			config.encodeBufferCount = 4;
			config.queueFrameCount = 4;
			config.keyFrameInterval = 60;

			BenchResult result = {};
			bool ok = Check(bench.Run(config, result), "bench run completed");

			ok &= Check(!result.faultedAtEnd, "encoder did not fault");
			ok &= Check(result.encoderStats.pendingFrames == 0, "no frames left pending");
			ok &= Check(result.encoderStats.lostFrames == 0, "no frames lost");
			ok &= Check(result.encoderStats.completedFrames == result.encoderStats.submittedFrames,
				"every submitted frame produced a packet");
			ok &= Check(result.encodedPackets == result.encoderStats.completedFrames,
				"every completed frame reached the app callback");
			ok &= Check(result.encodedPackets > 0, "packets were produced");
			ok &= Check(result.latencyUnmatched == 0, "every packet matched its enqueue timestamp");
			ok &= Check(result.keyFrames > 0, "keyframes were produced");

			PrintResult(config, result);
			EndCase(ok);
			return ok;
		}

		// Bug A (1) : 단발성 출력 실패는 프레임 1 장만 버리고 파이프라인이 계속 살아야 한다.
		bool TestRecoversFromTransientOutputFailure(EncodeBench& bench)
		{
			BeginCase("Bug A - transient output failures lose one frame each, pipeline survives");

			constexpr uint32_t kInjectAt = 40;
			constexpr uint32_t kInjectCount = 3;

			BenchConfig config;
			config.width = 1280;
			config.height = 720;
			config.frameCount = 180;
			config.targetFps = 120;
			config.encodeBufferCount = 4;
			config.queueFrameCount = 4;
			config.faultInjectAfterFrames = kInjectAt;
			config.faultInjectCount = kInjectCount;

			BenchResult result = {};
			bool ok = Check(bench.Run(config, result), "bench run completed");

			const uint32_t readFailed =
				result.errorCounts[static_cast<size_t>(NvEncErrorCode::OutputReadFailed)];

			ok &= Check(readFailed == kInjectCount, "app was notified once per lost frame");
			ok &= Check(result.encoderStats.lostFrames == kInjectCount,
				"exactly the injected frames were lost");
			ok &= Check(!result.faultedAtEnd, "encoder did NOT fault on a transient failure");
			ok &= Check(result.encoderStats.pendingFrames == 0,
				"pending count recovered to zero (slots were retired)");
			ok &= Check(result.encoderStats.completedFrames + result.encoderStats.lostFrames
				== result.encoderStats.submittedFrames,
				"submitted == completed + lost (no slot leaked)");
			ok &= Check(result.encodedPackets > kInjectAt,
				"encoding continued past the injection point");

			PrintResult(config, result);
			EndCase(ok);
			return ok;
		}

		// Bug A (2) : 실패가 계속되면 조용히 얼어붙는 대신 fault 를 통지하고 정지해야 한다.
		bool TestFaultsAfterSustainedOutputFailure(EncodeBench& bench)
		{
			BeginCase("Bug A - sustained output failures fault loudly instead of freezing");

			BenchConfig config;
			config.width = 1280;
			config.height = 720;
			config.frameCount = 180;
			config.targetFps = 120;
			config.encodeBufferCount = 4;
			config.queueFrameCount = 4;
			config.faultInjectAfterFrames = 20;
			config.faultInjectCount = 64;   // kMaxConsecutiveLostFrames 를 넘긴다

			const int64_t startTicks = QpcNow();
			BenchResult result = {};
			bool ok = Check(bench.Run(config, result), "bench run completed");
			const double elapsedSeconds = TicksToMilliseconds(QpcNow() - startTicks) / 1000.0;

			const uint32_t faultedCount =
				result.errorCounts[static_cast<size_t>(NvEncErrorCode::EncoderFaulted)];

			ok &= Check(result.faultedAtEnd, "encoder reported the faulted state");
			ok &= Check(faultedCount == 1, "app was notified exactly once about the fault");
			ok &= Check(result.encoderStats.pendingFrames == 0,
				"pending count was cleared so waiters cannot hang");

			// 수정 전에는 완료 스레드가 죽은 뒤에도 pending 이 남아
			// 종료 시 WaitForPendingFrames 가 타임아웃(20 초)까지 매달렸다.
			ok &= Check(elapsedSeconds < 15.0,
				"shutdown did not block on the 20s pending-frame timeout");

			// fault 이후에는 제출이 조용히 드롭되지 않고 결정적으로 거절돼야 한다.
			ok &= Check(result.enqueued > 0, "frames were enqueued before the fault");

			PrintResult(config, result);
			EndCase(ok);
			return ok;
		}

		// 동기 경로(DoEncode)도 같은 슬롯 회수 규칙을 따라야 한다.
		bool TestSyncPipeline(EncodeBench& bench)
		{
			BeginCase("sync pipeline (DoEncode) still works after the refactor");

			BenchConfig config;
			config.width = 1280;
			config.height = 720;
			config.frameCount = 60;
			config.targetFps = 0;            // 최대 속도
			config.encodeBufferCount = 2;
			config.asyncPipeline = false;

			BenchResult result = {};
			bool ok = Check(bench.Run(config, result), "bench run completed");

			ok &= Check(result.encodedPackets == config.frameCount, "every frame produced a packet");
			ok &= Check(!result.faultedAtEnd, "encoder did not fault");
			ok &= Check(result.encoderStats.pendingFrames == 0, "no frames left pending");
			ok &= Check(result.encoderStats.lostFrames == 0, "no frames lost");

			PrintResult(config, result);
			EndCase(ok);
			return ok;
		}

		// D3D11 multithread protection 없이 게이트만으로 컨텍스트를 보호하므로
		// Initialize / Destroy 도 게이트 안에서 돌아야 하고, 재귀 획득이 없어야 한다.
		// (실제 게이트는 SRWLOCK Exclusive 라 재귀 획득 시 데드락한다.)
		bool TestGateCoversInitAndDestroy(EncodeBench& bench)
		{
			BeginCase("gate covers Initialize / Destroy and never recurses");

			BenchConfig config;
			config.width = 1280;
			config.height = 720;
			config.frameCount = 120;
			config.targetFps = 120;
			config.encodeBufferCount = 4;
			config.queueFrameCount = 2;
			config.keyFrameInterval = 30;

			BenchResult result = {};
			bool ok = Check(bench.Run(config, result), "bench run completed");

			ok &= Check(result.gateRecursiveEnterCount == 0,
				"gate was never acquired recursively");
			ok &= Check(result.gateEnterCountDuringInit > 0,
				"Initialize acquired the gate (NVENC registers D3D11 resources there)");
			ok &= Check(result.gateEnterCountDuringDestroy > 0,
				"Destroy acquired the gate (unregister / unmap touch D3D11)");

			// 초기화 시퀀스 전체를 한 번의 획득으로 묶었는지 확인.
			// 단계별로 잡으면 렌더 스레드가 시퀀스 중간에 끼어들 수 있다.
			ok &= Check(result.gateEnterCountDuringInit == 1,
				"Initialize used exactly one gate acquisition (atomic sequence)");
			ok &= Check(result.gateEnterCountDuringDestroy <= 2,
				"Destroy used at most two acquisitions (Flush + resource teardown)");

			PrintResult(config, result);
			EndCase(ok);
			return ok;
		}

		// EncodeThread 는 출력을 회수하지 않으므로 동기 모드 엔코더와 조합하면
		// 아무도 드레인하지 않아 파이프라인이 조용히 정지한다. 초기화 단계에서 거절돼야 한다.
		bool TestEncodeThreadRejectsSyncEncoder(EncodeBench& bench)
		{
			BeginCase("EncodeThread rejects a sync-mode encoder (nothing would drain it)");

			bool ok = true;
			ID3D11Device* device = bench.GetDevice();
			SimpleContextGate gate;
			ReleaseCounter counter;

			EncodeFrameQueue queue;
			ok &= Check(queue.Initialize(2, CountingRelease, &counter), "queue initialized");

			// 동기 모드 엔코더는 거절돼야 한다.
			{
				D3D11NvEncoder syncEncoder;
				ok &= Check(syncEncoder.Initialize(device, 640, 480, 2, &gate, false),
					"sync-mode encoder initialized");
				ok &= Check(!syncEncoder.IsAsyncPipelineEnabled(),
					"encoder reports the async pipeline is off");

				EncodeThread encodeThread;
				ok &= Check(!encodeThread.Initialize(&queue, &syncEncoder),
					"EncodeThread refused the sync-mode encoder");

				syncEncoder.Destroy();
			}

			// async 모드 엔코더는 그대로 받아들여야 한다(가드가 과하게 막지 않는지).
			{
				D3D11NvEncoder asyncEncoder;
				ok &= Check(asyncEncoder.Initialize(device, 640, 480, 2, &gate, true),
					"async-mode encoder initialized");
				ok &= Check(asyncEncoder.IsAsyncPipelineEnabled(),
					"encoder reports the async pipeline is on");

				EncodeThread encodeThread;
				ok &= Check(encodeThread.Initialize(&queue, &asyncEncoder),
					"EncodeThread accepted the async-mode encoder");

				encodeThread.Shutdown();
				asyncEncoder.Destroy();
			}

			EndCase(ok);
			return ok;
		}

		// 네트워크 적응의 핵심 경로: 인코딩 중 비트레이트를 낮추면
		// 파이프라인을 멈추지 않고 즉시 반영돼야 한다.
		bool TestReconfigureBitrateWhileEncoding(EncodeBench& bench)
		{
			BeginCase("Reconfigure - bitrate changes take effect without stopping the pipeline");

			BenchConfig config;
			config.width = 1280;
			config.height = 720;
			config.frameCount = 400;
			config.targetFps = 120;
			config.encodeBufferCount = 4;
			config.queueFrameCount = 2;
			config.bitrateBps = 12000000;
			config.reconfigureAtFrame = 200;
			config.reconfigureBitrateBps = 2000000;

			BenchResult result = {};
			bool ok = Check(bench.Run(config, result), "bench run completed");

			ok &= Check(result.reconfigureApplied == 1, "reconfigure was applied");
			ok &= Check(result.reconfigureRejected == 0, "reconfigure was not rejected");
			ok &= Check(!result.faultedAtEnd, "encoder did not fault");
			ok &= Check(result.encoderStats.pendingFrames == 0, "no frames left pending");
			ok &= Check(result.encoderStats.lostFrames == 0, "no frames lost across the change");
			ok &= Check(result.encoderStats.completedFrames == result.encoderStats.submittedFrames,
				"every submitted frame still produced a packet");

			// 12 Mbps -> 2 Mbps 면 평균 프레임 크기가 눈에 띄게 줄어야 한다.
			const uint64_t afterFrames = result.encodedPackets - result.framesBeforeReconfigure;
			const uint64_t afterBytes = result.totalBytes - result.bytesBeforeReconfigure;
			const double beforeAvg = (result.framesBeforeReconfigure > 0)
				? static_cast<double>(result.bytesBeforeReconfigure)
					/ static_cast<double>(result.framesBeforeReconfigure)
				: 0.0;
			const double afterAvg = (afterFrames > 0)
				? static_cast<double>(afterBytes) / static_cast<double>(afterFrames)
				: 0.0;

			ok &= Check(afterFrames > 50, "enough frames were encoded after the change to compare");
			ok &= Check(afterAvg > 0.0 && afterAvg < beforeAvg * 0.75,
				"average packet size dropped after lowering the bitrate");

			PrintResult(config, result);
			EndCase(ok);
			return ok;
		}

		// [init] 필드를 바꾸려는 Reconfigure 는 조용히 무시되거나 부분 적용되면 안 된다.
		// 거절하고 이유를 알려야 한다.
		bool TestReconfigureRejectsInitOnlyFields(EncodeBench& bench)
		{
			BeginCase("Reconfigure - init-only fields are rejected, not partially applied");

			bool ok = true;
			SimpleContextGate gate;

			NvEncConfig config;
			config.width = 640;
			config.height = 480;
			config.encodeBufferCount = 2;
			config.averageBitrateBps = 4000000;

			D3D11NvEncoder encoder;
			ok &= Check(encoder.Initialize(bench.GetDevice(), config, &gate), "encoder initialized");

			// 런타임 필드만 바꾸면 통과해야 한다.
			NvEncConfig live = {};
			encoder.GetConfig(live);
			live.averageBitrateBps = 1500000;
			ok &= Check(encoder.Reconfigure(live, false) == NvEncReconfigureResult::Applied,
				"bitrate change is accepted");

			// 같은 값을 다시 넣으면 할 일이 없다.
			ok &= Check(encoder.Reconfigure(live, false) == NvEncReconfigureResult::NoChange,
				"an identical config reports NoChange");

			// [init] 필드는 거절돼야 한다.
			NvEncConfig resized = live;
			resized.width = 800;
			ok &= Check(encoder.Reconfigure(resized, false) == NvEncReconfigureResult::InitOnlyFieldChanged,
				"resolution change is rejected");

			NvEncConfig retuned = live;
			retuned.latencyMode = NvEncLatencyMode::Quality;
			ok &= Check(encoder.Reconfigure(retuned, false) == NvEncReconfigureResult::InitOnlyFieldChanged,
				"latency mode change is rejected");

			NvEncConfig reprofiled = live;
			reprofiled.profile = NvEncH264Profile::Baseline;
			ok &= Check(encoder.Reconfigure(reprofiled, false) == NvEncReconfigureResult::InitOnlyFieldChanged,
				"profile change is rejected");

			NvEncConfig regop = live;
			regop.enableIntraRefresh = !live.enableIntraRefresh;
			ok &= Check(encoder.Reconfigure(regop, false) == NvEncReconfigureResult::InitOnlyFieldChanged,
				"GOP structure change is rejected");

			// 거절 후에도 이전 설정이 그대로여야 한다(부분 적용 금지).
			NvEncConfig afterRejects = {};
			encoder.GetConfig(afterRejects);
			ok &= Check(afterRejects.width == 640 && afterRejects.height == 480,
				"resolution was left untouched by the rejected calls");
			ok &= Check(afterRejects.averageBitrateBps == 1500000,
				"the last accepted bitrate is still in effect");
			ok &= Check(afterRejects.latencyMode == live.latencyMode
				&& afterRejects.profile == live.profile
				&& afterRejects.enableIntraRefresh == live.enableIntraRefresh,
				"init-only fields were left untouched");

			// 잘못된 값도 거절돼야 한다.
			NvEncConfig zeroBitrate = live;
			zeroBitrate.averageBitrateBps = 0;
			ok &= Check(encoder.Reconfigure(zeroBitrate, false) == NvEncReconfigureResult::InvalidConfig,
				"a zero bitrate is rejected");

			encoder.Destroy();

			EndCase(ok);
			return ok;
		}

		// 같은 하네스 인스턴스로 세션을 반복해 열고 닫는다(재시작 경로).
		bool TestRepeatedSessions(EncodeBench& bench)
		{
			BeginCase("repeated encoder sessions on the same device");

			BenchConfig config;
			config.width = 1280;
			config.height = 720;
			config.frameCount = 60;
			config.targetFps = 0;
			config.encodeBufferCount = 4;
			config.queueFrameCount = 4;

			bool ok = true;
			for (uint32_t round = 0; round < 3; ++round)
			{
				BenchResult result = {};
				char label[128] = {};

				const bool ran = bench.Run(config, result);

				::sprintf_s(label, "round %u: Run() succeeded", round);
				ok &= Check(ran, label);

				::sprintf_s(label, "round %u: produced all %u packets (got %llu)",
					round, config.frameCount,
					static_cast<unsigned long long>(result.encodedPackets));
				ok &= Check(result.encodedPackets == config.frameCount, label);

				::sprintf_s(label, "round %u: no fault, nothing pending", round);
				ok &= Check(!result.faultedAtEnd && result.encoderStats.pendingFrames == 0, label);
			}

			EndCase(ok);
			return ok;
		}

		// ---------- 디코더 ----------

		// 왕복이 성립하는지부터 본다. 엔코더가 만든 비트스트림을 디코더가
		// 한 장도 흘리지 않고 되돌려줘야 한다. 이게 깨지면 아래 케이스들은
		// 전부 의미가 없다.
		bool TestRoundTripDeliversEveryFrame(RoundTripBench& roundTrip)
		{
			BeginCase("round trip delivers every encoded frame");

			RoundTripConfig config;
			config.frameCount = 120;
			config.targetFps = 60;

			RoundTripResult result = {};
			bool ok = Check(roundTrip.Run(config, result), "Run() succeeded");

			ok &= Check(result.encodedPackets == config.frameCount,
				"encoder produced one packet per frame");
			ok &= Check(result.decoderStats.parsedPackets == result.encodedPackets,
				"decoder parsed every packet");
			ok &= Check(result.decoderStats.deliveredFrames == result.encodedPackets,
				"decoder delivered every frame");
			ok &= Check(result.decoderStats.droppedPoolExhausted == 0
				&& result.decoderStats.droppedNotConsumed == 0
				&& result.decoderStats.droppedDisplayFailed == 0,
				"nothing dropped");
			ok &= Check(!result.decoderFaulted && !result.encoderFaulted, "neither side faulted");
			ok &= Check(result.decodedWidth == config.width && result.decodedHeight == config.height,
				"decoded texture matches the encoded resolution");

			EndCase(ok);
			return ok;
		}

		// timestamp 는 앱이 디코딩 결과를 원본 프레임과 짝짓는 유일한 끈이다.
		// 예전에는 CUVID_PKT_TIMESTAMP 플래그만 세우고 값을 넣지 않아 늘 0 이었고,
		// 그래서 왕복 지연 측정이 통째로 엉터리 값을 냈다.
		// 지연 샘플이 프레임 수만큼 잡혔다는 것은 timestamp 가 살아 돌아왔다는 뜻이다.
		bool TestTimestampSurvivesTheRoundTrip(RoundTripBench& roundTrip)
		{
			BeginCase("timestamp survives encode -> decode");

			RoundTripConfig config;
			config.frameCount = 120;
			config.targetFps = 60;

			RoundTripResult result = {};
			bool ok = Check(roundTrip.Run(config, result), "Run() succeeded");

			// 지연 샘플은 디코딩된 프레임의 timestamp 가 투입 기록과 맞을 때만 쌓인다.
			ok &= Check(result.latency.sampleCount == result.decodedFrames,
				"every decoded frame matched its encode-submit record");
			ok &= Check(result.latency.sampleCount > 0, "at least one sample");

			// 720p60 왕복이 한 프레임 간격(16.7ms)을 넘으면 저지연이라 할 수 없다.
			// 값이 0 에 붙어도 이상하다. timestamp 가 다시 죽었다는 뜻이다.
			ok &= Check(result.latency.p50Ms > 0.05 && result.latency.p50Ms < 16.7,
				"p50 round-trip latency is inside one frame interval");

			EndCase(ok);
			return ok;
		}

		// 앱이 슬롯 수보다 많은 프레임을 붙잡으면 디코더는 새 프레임을 쓸 곳이 없다.
		// 이때 아무 슬롯에나 덮어쓰면 앱이 보고 있는 텍스처가 찢어진다.
		// 덮어쓰는 대신 버리고, 계속 버려지면 조용히 멈추지 말고 fault 를 내야 한다.
		bool TestPoolExhaustionDropsInsteadOfCorrupting(RoundTripBench& roundTrip)
		{
			BeginCase("pool exhaustion drops frames instead of overwriting held ones");

			RoundTripConfig config;
			config.frameCount = 200;
			config.targetFps = 60;
			config.decodeSlotCount = 8;
			config.holdFrameCount = 12;   // 슬롯보다 많이 붙잡는다

			RoundTripResult result = {};
			bool ok = Check(roundTrip.Run(config, result), "Run() succeeded");

			ok &= Check(result.decoderStats.deliveredFrames <= config.decodeSlotCount,
				"delivered no more frames than there are slots");
			ok &= Check(result.decoderStats.droppedPoolExhausted > 0,
				"reported the exhaustion instead of overwriting");
			ok &= Check(result.decoderErrorCounts[
				static_cast<size_t>(NvDecErrorCode::OutputPoolExhausted)] > 0,
				"raised OutputPoolExhausted through the error callback");
			ok &= Check(result.decoderFaulted, "faulted after the loss ran on");

			// 붙잡고 있던 프레임은 종료 시 전부 반납되어야 한다.
			// 남아 있으면 Destroy 가 슬롯을 정리하지 못한다.
			ok &= Check(result.decoderStats.framesHeldByApp == 0,
				"every held frame was returned before teardown");

			EndCase(ok);
			return ok;
		}

		// 슬롯 수는 2 의 거듭제곱이어야 한다. 슬롯 색인이 & (count - 1) 이기 때문이다.
		// 1 개면 앱이 한 장 들고 있는 동안 쓸 슬롯이 없어 전부 버려진다.
		bool TestDecoderRejectsInvalidSlotCounts(RoundTripBench& roundTrip)
		{
			BeginCase("decoder rejects invalid output slot counts");

			ID3D11Device* device = roundTrip.GetDevice();
			bool ok = Check(device != nullptr, "device available");
			if (!ok)
			{
				EndCase(false);
				return false;
			}

			SimpleContextGate gate;

			const uint32_t badCounts[] = { 0U, 1U, 3U, 6U, 64U };
			for (uint32_t badCount : badCounts)
			{
				NvDecConfig config;
				config.outputSlotCount = badCount;

				D3D11NvDecoder decoder;
				const bool initialized = decoder.Initialize(device, config, &gate);
				decoder.Destroy();

				char label[128] = {};
				::sprintf_s(label, "outputSlotCount %u rejected", badCount);
				ok &= Check(!initialized, label);
			}

			// 정상값은 통과해야 한다. 위 검사가 무조건 false 를 내는 게 아님을 확인한다.
			NvDecConfig goodConfig;
			goodConfig.outputSlotCount = 8;

			D3D11NvDecoder decoder;
			const bool initialized = decoder.Initialize(device, goodConfig, &gate);
			ok &= Check(initialized, "outputSlotCount 8 accepted");
			decoder.Destroy();

			EndCase(ok);
			return ok;
		}

		// 게이트는 재귀 획득을 허용하지 않는다. 이미 잡은 스레드가 다시 잡으면
		// 실제 SRWLOCK 게이트에서는 그 자리에서 데드락한다.
		// 하네스 게이트는 죽는 대신 세어 두므로, 그 수가 0 이어야 한다.
		bool TestDecoderNeverReentersTheGate(RoundTripBench& roundTrip)
		{
			BeginCase("decoder never re-enters the context gate");

			RoundTripConfig config;
			config.frameCount = 150;
			config.targetFps = 60;

			// 렌더 스레드가 게이트를 계속 빼앗는 상황까지 겹친다.
			config.contendHz = 120;
			config.contendMicroseconds = 3000;

			RoundTripResult result = {};
			bool ok = Check(roundTrip.Run(config, result), "Run() succeeded");

			ok &= Check(result.gateEnterCount > 0, "the gate was actually used");
			ok &= Check(result.gateRecursiveEnterCount == 0,
				"no recursive acquisition (would deadlock a real gate)");
			ok &= Check(result.contendAcquireCount > 0, "the fake render thread ran");
			ok &= Check(!result.decoderFaulted && !result.encoderFaulted,
				"both sides survived the contention");
			ok &= Check(result.decoderStats.deliveredFrames == result.encodedPackets,
				"contention cost latency but not frames");

			EndCase(ok);
			return ok;
		}

		// 엔코더와 마찬가지로, 디코더도 세션을 반복해서 열고 닫을 수 있어야 한다.
		// Destroy 가 슬롯이나 CUDA 자원을 흘리면 두 번째 라운드부터 무너진다.
		bool TestRepeatedDecodeSessions(RoundTripBench& roundTrip)
		{
			BeginCase("repeated decode sessions stay clean");

			bool ok = true;
			for (uint32_t round = 0; round < 3; ++round)
			{
				RoundTripConfig config;
				config.frameCount = 60;

				// fps 0 (무제한) 으로 돌리면 latest-wins 큐가 거의 다 버려서
				// 라운드당 1 장만 인코딩된다. 그러면 "1 of 1" 로 통과해 버려
				// 검사가 사실상 아무것도 확인하지 못한다. 실제로 흘려보낸다.
				config.targetFps = 120;

				RoundTripResult result = {};
				char label[160] = {};

				::sprintf_s(label, "round %u: Run() succeeded", round);
				ok &= Check(roundTrip.Run(config, result), label);

				::sprintf_s(label, "round %u: encoded %llu of %u frames (test is not vacuous)",
					round, static_cast<unsigned long long>(result.encodedPackets), config.frameCount);
				ok &= Check(result.encodedPackets >= config.frameCount / 2U, label);

				::sprintf_s(label, "round %u: delivered %llu of %llu",
					round,
					static_cast<unsigned long long>(result.decoderStats.deliveredFrames),
					static_cast<unsigned long long>(result.encodedPackets));
				ok &= Check(result.decoderStats.deliveredFrames == result.encodedPackets, label);

				::sprintf_s(label, "round %u: no fault, nothing held", round);
				ok &= Check(!result.decoderFaulted && result.decoderStats.framesHeldByApp == 0, label);
			}

			EndCase(ok);
			return ok;
		}
	}

	int RunSelfTest()
	{
		printf_s("NvCodec self test\n");
		printf_s("=========================================================\n");

		g_passed = 0;
		g_failed = 0;

		EncodeBench bench;
		if (!bench.Setup(1280, 720, 8))
		{
			printf_s("[FATAL] Setup failed. A hardware D3D11 device with NVENC is required.\n");
			return 2;
		}

		TestQueueCanBeReinitialized();
		TestRejectsInvalidBufferCounts(bench);
		TestCleanShutdownDrainsEveryFrame(bench);
		TestRecoversFromTransientOutputFailure(bench);
		TestFaultsAfterSustainedOutputFailure(bench);
		TestSyncPipeline(bench);
		TestGateCoversInitAndDestroy(bench);
		TestReconfigureBitrateWhileEncoding(bench);
		TestReconfigureRejectsInitOnlyFields(bench);
		TestEncodeThreadRejectsSyncEncoder(bench);
		TestRepeatedSessions(bench);

		// ---- 디코더 ----
		// 왕복 하네스는 엔코더와 디코더를 같은 디바이스, 같은 게이트로 돌린다.
		// 실제 클라이언트가 렌더와 디코드를 한 컨텍스트에서 하는 구성과 같다.
		RoundTripBench roundTrip;
		if (roundTrip.Setup(1280, 720))
		{
			TestRoundTripDeliversEveryFrame(roundTrip);
			TestTimestampSurvivesTheRoundTrip(roundTrip);
			TestPoolExhaustionDropsInsteadOfCorrupting(roundTrip);
			TestDecoderRejectsInvalidSlotCounts(roundTrip);
			TestDecoderNeverReentersTheGate(roundTrip);
			TestRepeatedDecodeSessions(roundTrip);
		}
		else
		{
			printf_s("\n[SKIP] Round trip setup failed. Decoder cases did not run.\n");
			++g_failed;
		}

		printf_s("\n=========================================================\n");
		printf_s(" self test: %u passed, %u failed\n", g_passed, g_failed);
		printf_s("=========================================================\n");

		return (g_failed == 0) ? 0 : 1;
	}
}
