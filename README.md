# NvCodec

Direct3D 11 텍스처를 H.264로 인코딩하고 H.264 비트스트림을 D3D11에서 사용할 수 있는 프레임으로 디코딩하는 Windows x64 DLL이다. NVIDIA Video Codec SDK의 NVENC와 NVDEC를 사용한다.

- `NvEncode/`에서 D3D11 Video Processor로 입력 텍스처를 NV12로 변환하고 NVENC로 인코딩한다.
- `NvDecode/`에서 NVDEC로 비트스트림을 디코딩하고 CUDA를 통해 출력 프레임을 처리한다.

## 빌드 환경

- Visual Studio 2026의 **C++를 사용한 데스크톱 개발** 워크로드와 MSVC v145
- Windows 10 또는 11 x64와 Windows SDK
- C++20 (MSVC `/std:c++20`, CUDA `-std=c++20`)
- NVIDIA Video Codec SDK 13.0.37 (프로젝트의 `Nvidia Video Codec SDK/`에 헤더 및 링크 라이브러리 포함)
- NVIDIA CUDA Toolkit 13.4.3과 Visual Studio 통합 구성 요소
- 실행 시 NVENC와 NVDEC를 지원하는 NVIDIA GPU 및 드라이버

CUDA Toolkit의 설치 버전은 13.4.3이며 Visual Studio 빌드 사용자 지정 파일은 `CUDA 13.4.props`와 `CUDA 13.4.targets`를 사용한다. 기본 설치 경로는 `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4`이다. 설치 후 Visual Studio 또는 터미널을 다시 열어 `CUDA_PATH_V13_4` 환경 변수를 반영한다.

## 저장소 배치와 빌드

`Core`는 빌드에 필요한 프로젝트이며, `D3D11EngineInterface`는 D3D11 즉시 컨텍스트 접근을 조율하는 게이트 인터페이스의 헤더를 제공한다. NvCodec을 별도 저장소로 내려받는 경우에도 아래와 같이 같은 상위 폴더에 배치한다.

```text
Module/
├─ Core/
│  └─ Core/Core.vcxproj
├─ D3D11EngineInterface/
│  └─ ID3D11ImmediateContextGate.h
└─ NvCodec/
   └─ NvCodec.sln
```

Visual Studio 2026에서 `NvCodec.sln`을 열고 `Debug|x64` 또는 `Release|x64`로 빌드한다. Visual Studio 2026용 개발자 명령 프롬프트에서도 빌드할 수 있다.

```powershell
msbuild .\NvCodec.sln /m /p:Configuration=Release /p:Platform=x64
```

빌드한 DLL은 `x64/Release/NvCodec.dll` 또는 `x64/Debug/NvCodec.dll`에서 확인할 수 있다.
