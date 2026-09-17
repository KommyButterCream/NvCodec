#pragma once

#include <stdint.h>
#include <stddef.h>

// 코덱은 H.264 고정이다. 엔코더와 짝을 맞춘다.

// 디코더가 앱에 통지하는 오류 종류.
// OutputPoolExhausted / OutputNotConsumed 는 프레임 유실이고 디코딩은 계속된다.
// 나머지는 파이프라인이 정지(faulted)했음을 의미한다.
enum class NvDecErrorCode : uint32_t
{
	None = 0,
	OutputPoolExhausted,  // 앱이 ReleaseFrame 을 하지 않아 쓸 슬롯이 없다 (프레임 유실)
	OutputNotConsumed,    // 앱이 AcquireFrame 을 하지 않아 밀린 프레임을 건너뛰었다
	ParseFailed,          // cuvidParseVideoData 실패. 비트스트림이 깨졌을 수 있다
	DecodeFailed,         // cuvidDecodePicture 실패
	DisplayFailed,        // 디코딩 결과를 텍스처로 옮기지 못했다 (프레임 유실)
	ReconfigureFailed,    // 해상도 변경 처리 실패 (복구 불가)
	DecoderFaulted,       // 실패가 연속 누적되어 세션을 포기
};

// 진단 및 벤치마크용 누적 통계.
struct NvDecStats
{
	uint64_t parsedPackets = 0;          // Parse 호출 성공 횟수
	uint64_t dequeuedFrames = 0;         // 유입 큐에서 디코드 스레드가 꺼낸 수
	uint64_t droppedInputQueue = 0;      // 유입 큐가 가득 차 버린 수
	uint64_t packetsFailed = 0;          // Parse 실패 횟수 (디코드 스레드를 쓸 때만)
	uint64_t decodedFrames = 0;          // 텍스처까지 완성된 프레임 수
	uint64_t deliveredFrames = 0;        // AcquireFrame 으로 앱에 나간 수
	uint64_t droppedPoolExhausted = 0;   // 앱이 반납하지 않아 버린 수
	uint64_t droppedNotConsumed = 0;     // 앱이 가져가지 않아 건너뛴 수
	uint64_t droppedDisplayFailed = 0;   // 변환/복사 실패로 버린 수
	uint32_t framesHeldByApp = 0;        // 현재 앱이 들고 있는 슬롯 수
	bool faulted = false;
};

// 디코더 설정.
//
// 디코더는 해상도를 스트림에서 읽어오므로 엔코더처럼 런타임 재설정할 항목이 없다.
// 전부 Initialize 에서만 정한다.
// 디코더에 넣는 비트스트림 한 덩어리.
//
// data 는 EnqueueFrame 이 반환할 때까지만 유효하면 된다 - 큐가 자기 버퍼로
// 복사해 간다. 인코더 입력(NvEncInputFrame)이 참조만 싣는 것과 다른 점이다.
// 그래서 디코더에는 반납 콜백이 없다.
struct NvDecInputFrame
{
	const uint8_t* data = nullptr;
	size_t size = 0;
	uint64_t frameId = 0;
	uint64_t timestamp = 0;
	uint16_t frameType = 0;
};

struct NvDecConfig
{
	// 디코드 스레드가 꺼내 가는 유입 큐의 슬롯 수.
	uint32_t inputQueueDepth = 8;

	// 유입 큐가 슬롯마다 잡아 둘 버퍼 크기(바이트).
	// 큐가 패킷을 자기 버퍼로 복사하므로 가장 큰 패킷이 들어갈 수 있어야 한다.
	size_t maxPacketSize = 512 * 1024;

	// 출력 슬롯 수. 2 의 n 승이며 최소 2.
	//
	// 앱이 ReleaseFrame 하지 않고 동시에 들고 있을 수 있는 프레임 수를 결정한다.
	// 슬롯은 FIFO 로 재사용되므로, 앱이 프레임 하나를 오래 붙잡으면
	// 그 슬롯 차례가 돌아왔을 때 새 프레임이 버려진다.
	// 일반적인 렌더러는 한 번에 한 장만 들고 있으므로 8 이면 충분하다.
	uint32_t outputSlotCount = 8;

	// 출력 텍스처를 공유 텍스처로 만든다. 다른 D3D11 디바이스에서 열어 쓸 때.
	bool sharedOutputTextureMode = false;

	// 앱이 프레임을 못 따라갈 때 몇 장까지 밀리도록 둘지.
	// 이보다 밀리면 최신 프레임으로 건너뛴다(라이브 영상은 최신이 중요하다).
	// 건너뛴 수는 droppedNotConsumed 로 집계된다.
	uint32_t maxOutputLagFrames = 3;

	// NVDEC 파서가 관리할 디코드 서페이스 상한.
	uint32_t maxDecodeSurfaces = 20;

	// 이만큼 연속으로 프레임을 잃으면 세션을 포기하고 통지한다.
	uint32_t maxConsecutiveLostFrames = 32;
};
