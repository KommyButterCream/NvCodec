#pragma once

#include <stdint.h>

struct NvEncPacket
{
	const uint8_t* data = nullptr;
	uint32_t size = 0;
	uint64_t frameId = 0;
	uint64_t timestamp = 0;
	uint16_t frameType = 0;
	bool isKeyFrame = false;
};

// 엔코더가 앱에 통지하는 오류 종류.
// OutputReadFailed 는 프레임 1장 유실로 파이프라인은 계속 살아있고,
// 나머지는 파이프라인이 정지(faulted)했음을 의미한다.
enum class NvEncErrorCode : uint32_t
{
	None = 0,
	OutputReadFailed,   // 인코딩은 끝났지만 비트스트림 회수 실패 (프레임 유실, 복구됨)
	OutputTimeout,      // completion event 미신호. NVENC 가 슬롯을 잡고 있어 복구 불가
	OutputUnmapFailed,  // Input Resource Unmap 실패. 매핑이 누적되므로 복구 불가
	RingCorrupted,      // pending 프레임 링 상태 불일치
	EncoderFaulted,     // 유실이 연속 누적되어 세션을 포기
};

// 진단 및 벤치마크용 누적 통계.
struct NvEncStats
{
	uint64_t submittedFrames = 0;   // SubmitFrame 성공 횟수
	uint64_t completedFrames = 0;   // 비트스트림까지 정상 회수된 횟수
	uint64_t lostFrames = 0;        // 제출됐지만 결과를 못 받은 횟수
	uint32_t pendingFrames = 0;     // NVENC 에 제출됐고 아직 회수하지 않은 프레임 수
	bool faulted = false;           // 파이프라인 정지 여부
};
