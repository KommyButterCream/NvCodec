#include "SelfTest.h"

#include <stdio.h>

#include "EncodeBench.h"
#include "SimpleContextGate.h"
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
	}

	int RunSelfTest()
	{
		printf_s("NvEncode self test\n");
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
		TestEncodeThreadRejectsSyncEncoder(bench);
		TestRepeatedSessions(bench);

		printf_s("\n=========================================================\n");
		printf_s(" self test: %u passed, %u failed\n", g_passed, g_failed);
		printf_s("=========================================================\n");

		return (g_failed == 0) ? 0 : 1;
	}
}
