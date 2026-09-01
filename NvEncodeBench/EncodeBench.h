#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <stdint.h>

#include <string>

#include "LatencyStats.h"
#include "../NvEncode/EncodeFrameQueue.h"
#include "../NvEncode/NvEncPacket.h"
#include "../NvEncode/NvEncConfig.h"
#include "../NvEncode/NvEncPacket.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
class D3D11NvEncoder;

namespace Bench
{
	struct BenchConfig
	{
		uint32_t width = 1920;
		uint32_t height = 1080;
		uint32_t frameCount = 300;

		// 0 이면 스로틀 없이 최대 속도로 밀어넣는다(인코더 상한 측정용).
		uint32_t targetFps = 60;

		uint32_t encodeBufferCount = 4;   // 2 의 n 승, 최소 2
		uint32_t queueFrameCount = 4;     // 2 의 n 승, 최소 2
		uint32_t sourcePoolCount = 8;     // 생산자 텍스처 풀 크기(= 패턴 종류 수)
		uint32_t keyFrameInterval = 0;    // 0 이면 키프레임 요청 안 함

		// false 면 큐 펌프 없이 호출 스레드에서 DoEncode 로 동기 인코딩한다.
		bool asyncPipeline = true;

		// 콜백에서 소비하는 CPU 시간을 인위적으로 만든다(마이크로초).
		// 실제 앱은 콜백에서 브로드캐스트(청크 분할 + 네트워크 패킷으로 memcpy)를
		// 수행하므로 CPU 바운드 작업이다. Sleep 이 아니라 스핀으로 흉내낸다.
		// 이 시간이 파이프라인 처리량/지연에 얼마나 영향을 주는지 재기 위한 것.
		uint32_t callbackDelayMicroseconds = 0;

		// 엔코더 설정. 하네스는 이 값을 그대로 D3D11NvEncoder::Initialize 에 넘긴다.
		NvEncLatencyMode latencyMode = NvEncLatencyMode::UltraLow;
		NvEncH264Profile profile = NvEncH264Profile::High;
		bool enableIntraRefresh = true;
		uint32_t intraRefreshPeriodFrames = 60;
		uint32_t gopLengthFrames = 60;
		uint32_t bitrateBps = 5000000;
		uint32_t vbvBufferSizeBits = 0;   // 0 = 1 프레임 자동

		// 런타임 재설정 검증용.
		// reconfigureAtFrame 번째 프레임에서 비트레이트를 reconfigureBitrateBps 로 바꾼다.
		uint32_t reconfigureAtFrame = 0;
		uint32_t reconfigureBitrateBps = 0;

		// 게이트를 경합시키는 가짜 렌더 스레드.
		// contendHz 회/초로 게이트를 잡고 contendMicroseconds 동안 점유한다.
		// D3D11 multithread protection 없이 게이트만으로 컨텍스트를 보호하는
		// 구성에서, 렌더 스레드가 있을 때 엔코더가 데드락하지 않고
		// 얼마나 느려지는지 재기 위한 것.
		uint32_t contendHz = 0;
		uint32_t contendMicroseconds = 0;

		// 출력 실패 복구 경로 검증용.
		// faultInjectAfterFrames 번째 프레임 투입 직전에
		// 다음 faultInjectCount 번의 비트스트림 회수를 강제로 실패시킨다.
		uint32_t faultInjectAfterFrames = 0;
		uint32_t faultInjectCount = 0;

		std::string csvPath;
	};

	struct BenchResult
	{
		bool initialized = false;

		uint64_t enqueued = 0;            // EnqueueLatest 성공
		uint64_t enqueueRejected = 0;     // EnqueueLatest 실패(큐가 슬롯을 못 줌)
		uint64_t producerStalled = 0;     // 소스 풀에 빈 슬롯이 없어 건너뛴 프레임
		uint64_t encodedPackets = 0;      // 콜백으로 받은 패킷 수
		uint64_t keyFrames = 0;
		uint64_t totalBytes = 0;
		uint64_t latencyUnmatched = 0;    // 제출 시각을 못 찾은 패킷(정상이면 0)

		uint32_t queueDropCount = 0;      // 큐에서 최신 프레임에 밀려 버려진 수
		uint32_t queueProcessCount = 0;

		NvEncStats encoderStats = {};
		bool faultedAtEnd = false;
		uint32_t errorCounts[8] = {};     // NvEncErrorCode 별 통지 횟수

		double wallSeconds = 0.0;
		uint64_t gateEnterCount = 0;
		uint64_t contendAcquireCount = 0;
		double contendMaxWaitMs = 0.0;
		uint32_t reconfigureApplied = 0;
		uint32_t reconfigureRejected = 0;
		uint64_t bytesBeforeReconfigure = 0;
		uint64_t framesBeforeReconfigure = 0;
		uint64_t gateRecursiveEnterCount = 0;   // 0 이 아니면 실제 게이트에서 데드락한다
		uint64_t gateEnterCountDuringInit = 0;  // Initialize 구간의 게이트 획득 수
		uint64_t gateEnterCountDuringDestroy = 0;

		LatencySummary latency = {};
	};

	const char* ToString(NvEncErrorCode errorCode);
	void PrintResult(const BenchConfig& config, const BenchResult& result);

	// 한 번의 인코딩 세션을 구성하고 돌린다.
	// 같은 인스턴스로 Run 을 여러 번 호출해 재초기화 경로를 검증할 수 있다.
	class EncodeBench
	{
	public:
		EncodeBench();
		~EncodeBench();

		EncodeBench(const EncodeBench&) = delete;
		EncodeBench& operator=(const EncodeBench&) = delete;

		// D3D11 디바이스와 패턴 텍스처를 준비한다. Run 전에 한 번 호출한다.
		bool Setup(uint32_t width, uint32_t height, uint32_t sourcePoolCount);
		void Teardown();

		bool Run(const BenchConfig& config, BenchResult& result);

		ID3D11Device* GetDevice() const { return m_device; }

	private:
		struct Impl;

		bool RunAsync(const BenchConfig& config, BenchResult& result, D3D11NvEncoder& encoder);
		bool RunSync(const BenchConfig& config, BenchResult& result, D3D11NvEncoder& encoder);

		static void OnReleaseFrame(EncodeFrameQueue::InputFrameHandle& frameHandle, void* userData);
		static void OnEncodedFrame(const NvEncPacket& packet, void* userData);
		static void OnEncoderError(NvEncErrorCode errorCode, void* userData);

	private:
		Impl* m_impl = nullptr;

		ID3D11Device* m_device = nullptr;
		ID3D11DeviceContext* m_context = nullptr;
	};
}
