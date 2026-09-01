#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <stdint.h>

#include "LatencyStats.h"
#include "../NvDecode/NvDecConfig.h"
#include "../NvEncode/NvEncConfig.h"
#include "../NvEncode/NvEncPacket.h"

struct ID3D11Device;

namespace Bench
{
	// 엔코드 -> 디코드 왕복 설정.
	//
	// 엔코더 하네스가 만든 H.264 비트스트림을 그대로 디코더에 먹여
	// 완결된 루프를 만든다. 디코더만 단독으로 검증하려면 유효한 비트스트림을
	// 어딘가에서 구해야 하는데, 엔코더가 바로 옆에 있으므로 그걸 쓴다.
	struct RoundTripConfig
	{
		uint32_t width = 1280;
		uint32_t height = 720;
		uint32_t frameCount = 240;
		uint32_t targetFps = 60;

		uint32_t encodeBufferCount = 4;
		uint32_t decodeSlotCount = 8;
		uint32_t bitrateBps = 8000000;

		// 디코더가 준 프레임을 이만큼 붙잡고 있다가 반납한다.
		// 앱이 렌더링하느라 프레임을 들고 있는 상황을 흉내낸다.
		// 슬롯 수보다 크게 잡으면 풀 고갈이 재현된다.
		uint32_t holdFrameCount = 1;

		// 반납을 아예 하지 않는다. 슬롯 고갈 경로 검증용.
		bool leakFrames = false;

		// 게이트를 경합시키는 가짜 렌더 스레드.
		uint32_t contendHz = 0;
		uint32_t contendMicroseconds = 0;
	};

	struct RoundTripResult
	{
		bool initialized = false;

		uint64_t encodedPackets = 0;
		uint64_t encodedBytes = 0;
		uint64_t decodedFrames = 0;     // 디코더 콜백으로 받은 수
		uint64_t decodeQueueRejected = 0;

		// 디코드 큐가 가득 차서 버린 수. 큐는 latest-wins 라 조용히 버린다.
		// 이걸 세지 않으면 "인코딩 300, 디코딩 293" 이 디코더 결함처럼 보인다.
		uint64_t decodeQueueDropped = 0;

		NvEncStats encoderStats = {};
		NvDecStats decoderStats = {};

		uint32_t decoderErrorCounts[8] = {};
		bool encoderFaulted = false;
		bool decoderFaulted = false;

		uint64_t gateRecursiveEnterCount = 0;
		uint64_t gateEnterCount = 0;

		// 가짜 렌더 스레드가 게이트를 가져간 횟수와, 게이트를 얻기까지 기다린 최대 시간.
		uint64_t contendAcquireCount = 0;
		double contendMaxWaitMs = 0.0;

		double wallSeconds = 0.0;

		// 엔코드 투입 -> 디코드 출력까지의 왕복 지연.
		LatencySummary latency = {};

		// 디코딩된 텍스처의 실제 크기. 엔코드 해상도와 일치해야 한다.
		uint32_t decodedWidth = 0;
		uint32_t decodedHeight = 0;
	};

	void PrintRoundTripResult(const RoundTripConfig& config, const RoundTripResult& result);

	// 엔코더와 디코더를 같은 D3D11 디바이스, 같은 게이트로 돌린다.
	// 실제 클라이언트가 렌더와 디코드를 한 컨텍스트에서 하는 구성과 같다.
	class RoundTripBench
	{
	public:
		RoundTripBench();
		~RoundTripBench();

		RoundTripBench(const RoundTripBench&) = delete;
		RoundTripBench& operator=(const RoundTripBench&) = delete;

		bool Setup(uint32_t width, uint32_t height);
		void Teardown();

		bool Run(const RoundTripConfig& config, RoundTripResult& result);

		ID3D11Device* GetDevice() const;

		// 콜백 트램폴린이 접근해야 해서 공개해 둔다. 정의는 .cpp 안에만 있다.
		struct Impl;

	private:
		Impl* m_impl = nullptr;
	};
}
