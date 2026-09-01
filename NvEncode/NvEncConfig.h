#pragma once

#include <stdint.h>

// 코덱은 H.264 고정이다. 다른 코덱은 추후 별도 설정으로 확장한다.

// 지연과 화질 사이의 선택. NVENC 프리셋 + 튜닝 조합으로 매핑된다.
// Initialize 이후에는 바꿀 수 없다(프리셋 변경은 GOP 구조를 바꿀 수 있어
// nvEncReconfigureEncoder 가 거절한다).
enum class NvEncLatencyMode : uint8_t
{
	UltraLow = 0,   // 원격 조작. 룩어헤드/B프레임 없음. 기본값
	Low,            // 원격 시청. 화질을 조금 더 확보
	Quality,        // 녹화. 지연을 포기하고 화질 우선
};

// H.264 프로파일. Initialize 이후에는 바꿀 수 없다.
enum class NvEncH264Profile : uint8_t
{
	Baseline = 0,   // 호환성 최우선. CABAC 없음
	Main,
	High,           // 기본값. 같은 비트레이트에서 화질이 가장 좋다
};

// 레이트 컨트롤 방식. 런타임 변경 가능.
enum class NvEncRateControl : uint8_t
{
	ConstantBitrate = 0,   // CBR. 네트워크 대역이 고정일 때. 기본값
	VariableBitrate,       // VBR. targetQuality 를 함께 쓴다
	ConstantQP,            // 고정 QP. 진단용
};

// 엔코더 설정.
//
// 필드는 두 종류로 나뉜다.
//   [init]    Initialize 에서만 정할 수 있다. 바꾸려면 Destroy 후 재초기화.
//   [runtime] Reconfigure 로 인코딩 중에도 바꿀 수 있다.
//
// Reconfigure 는 [init] 필드가 달라지면 거절하고 이유를 로그로 남긴다.
struct NvEncConfig
{
	// ---------------- 해상도 ----------------

	// [init] 인코딩 해상도.
	uint32_t width = 0;
	uint32_t height = 0;

	// [init] 런타임 해상도 변경을 위한 상한.
	// 0 이면 width/height 와 동일하게 잡는다(여유 없음).
	// NVENC 가 이 크기 기준으로 내부 버퍼를 잡으므로 필요 이상으로 키우지 않는다.
	uint32_t maxWidth = 0;
	uint32_t maxHeight = 0;

	// ---------------- 파이프라인 ----------------

	// [init] 인코더 슬롯 수. 2 의 n 승이며 최소 2.
	// 부하 시 최악 지연이 이 값에 비례한다(지연 = 깊이 / 처리량).
	// 처리량은 2~8 에서 평평하므로 저지연 목적이면 2 가 유리하다.
	uint32_t encodeBufferCount = 4;

	// [init] false 면 완료 스레드를 만들지 않는다.
	// 호출자가 PrepareFrameForEncode + DoEncode 를 직접 돌려야 하고
	// EncodeThread 와는 조합할 수 없다.
	bool enableAsyncPipeline = true;

	// ---------------- 화질 / 코덱 ----------------

	// [init]
	NvEncLatencyMode latencyMode = NvEncLatencyMode::UltraLow;

	// [init]
	NvEncH264Profile profile = NvEncH264Profile::High;

	// [init] 주기적 IDR 대신 매 프레임의 일부 영역만 intra 로 인코딩한다.
	// IDR 은 P 프레임의 5~10 배 크기라 주기마다 지연이 튀는데,
	// intra refresh 는 프레임 크기를 균일하게 만들어 그 스파이크를 없앤다.
	// 손실 복구는 RequestKeyFrame 으로 필요할 때만 한다.
	bool enableIntraRefresh = true;

	// [init] intra refresh 가 화면을 한 바퀴 도는 데 걸리는 프레임 수.
	uint32_t intraRefreshPeriodFrames = 60;

	// [init] intra refresh 를 끌 때만 의미가 있다. IDR 주기(프레임).
	uint32_t gopLengthFrames = 60;

	// [init] 모든 키프레임 앞에 SPS/PPS 를 다시 넣는다.
	// 스트림 중간에 접속하는 수신자가 디코딩을 시작할 수 있게 해준다.
	bool repeatSequenceHeader = true;

	// ---------------- 레이트 컨트롤 (런타임 변경 가능) ----------------

	// [runtime] 표시 프레임레이트. 레이트 컨트롤과 VBV 계산의 기준이 된다.
	// 실제 투입 속도와 다르면 비트레이트가 그만큼 어긋난다.
	uint32_t frameRateNumerator = 60;
	uint32_t frameRateDenominator = 1;

	// [runtime]
	NvEncRateControl rateControl = NvEncRateControl::ConstantBitrate;

	// [runtime] 목표 비트레이트(bps).
	uint32_t averageBitrateBps = 5000000;

	// [runtime] VBR 상한(bps). 0 이면 averageBitrateBps 와 동일.
	uint32_t maxBitrateBps = 0;

	// [runtime] VBV(HRD) 버퍼 크기(bit). 인코더가 한 번에 몰아 쓸 수 있는 비트 예산.
	// 크면 복잡한 장면의 화질이 유지되지만 그 프레임이 커져 전송이 늦어진다.
	// 0 이면 저지연 기본값인 1 프레임 분량으로 자동 계산한다
	// (averageBitrateBps * frameRateDenominator / frameRateNumerator).
	uint32_t vbvBufferSizeBits = 0;

	// [runtime] QP 하한/상한. 둘 다 0 이면 적용하지 않는다.
	// CBR 에서 상한을 걸면 화질 바닥이 생기는 대신 비트레이트를 넘길 수 있다.
	uint8_t minQP = 0;
	uint8_t maxQP = 0;

	// [runtime] VBR 목표 품질(0~51, 0 이면 미적용). rateControl 이 VariableBitrate 일 때만.
	uint8_t targetQuality = 0;

	// [runtime] ConstantQP 모드에서 쓸 QP. rateControl 이 ConstantQP 일 때만.
	uint8_t constantQP = 26;

	// [runtime] 공간 적응 양자화. 평탄한 영역의 블로킹을 줄인다.
	bool enableAdaptiveQuantization = false;
};

// Reconfigure 가 거절한 이유.
enum class NvEncReconfigureResult : uint8_t
{
	Applied = 0,          // 적용됨
	NoChange,             // 바뀐 값이 없어 아무것도 하지 않음
	NotInitialized,       // 엔코더가 초기화되지 않았거나 fault 상태
	InitOnlyFieldChanged, // [init] 필드가 달라졌다. Destroy 후 재초기화가 필요
	InvalidConfig,        // 값 자체가 유효하지 않다
	DriverRejected,       // nvEncReconfigureEncoder 가 실패
};
