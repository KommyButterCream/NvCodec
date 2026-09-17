#pragma once

#include <stdint.h>

struct ID3D11Texture2D;

// 인코더에 넣는 입력 프레임 한 장.
//
// 가리키는 방법이 두 가지다.
//   texture      : 인코더와 같은 디바이스의 텍스처
//   sourceSlotId : 생산자가 공유한 입력 풀의 슬롯 번호 (texture 는 nullptr)
// 둘 중 하나는 반드시 채워야 한다.
//
// 이 핸들의 실제 자원은 앱 것이다. 인코더는 다 쓰거나 버린 시점에
// SetFrameReleaseCallback 으로 등록한 콜백으로 돌려준다.
struct NvEncInputFrame
{
	ID3D11Texture2D* texture = nullptr;
	int64_t sourceSlotId = -1;
	uint64_t frameId = 0ULL;
};

// 인코더가 입력 프레임을 다 썼거나 버렸을 때 부른다.
// 인코드 스레드 또는 유입 스레드에서 불리므로 블로킹 작업을 하면 안 된다.
using NvEncFrameReleaseCallback = void (*)(NvEncInputFrame& frame, void* userData);

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
	SlotRingCorrupted,  // pending 프레임 링 상태 불일치
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

	// 인코드 스레드를 쓸 때만 채워진다.
	//
	// droppedInputQueue 는 유입 시점, 나머지는 큐에서 꺼낸 뒤의 드롭이다.
	uint64_t dequeuedFrames = 0;        // 유입 큐에서 인코드 스레드가 꺼낸 수
	uint64_t droppedInputQueue = 0;     // 유입 큐가 최신 프레임으로 교체하며 버렸다
	uint64_t droppedNoEncoderSlot = 0;  // 제출 시점에 빈 슬롯이 없었다
	uint64_t droppedPrepareFailed = 0;  // NV12 변환/매핑 실패
	uint64_t droppedSubmitFailed = 0;   // SubmitFrame 실패
};
