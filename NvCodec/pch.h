#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN

// C4324: "맞춤 지정자 때문에 구조체가 채워졌습니다"
//
// alignas(64) 로 링 커서와 큐 커서를 서로 다른 캐시 라인에 떼어놓은 결과다.
// 패딩이 생기는 것이 목적이므로 경고가 알려줄 것이 없다.
// 정렬 보장용 alignas(4) / alignas(8) 은 패딩을 만들지 않아 여기 해당하지 않는다.
#pragma warning(disable : 4324)

#include <Windows.h>
#include <stdint.h>
#include <d3d11.h>
#include <dxgiformat.h>

#include "../../Core/DirectX/DxSafeRelease.h" // for SafeRelease

using namespace Core::DirectX;