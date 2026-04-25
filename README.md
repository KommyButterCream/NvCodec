# NvCodec
D3D11-based NvCodec DLL library

# Info
A D3D11-based DLL library for NVIDIA Video Codec SDK integration.
Provides H.264 encode/decode functionality built on top of Direct3D 11, CUDA, and the NVIDIA Video Codec SDK.

# Dependencies
- [Core](../Core) as a submodule
- NVIDIA Video Codec SDK
- NVIDIA CUDA Toolkit

# Build Environment
- C++20
- MSVC (Visual Studio 2022)
- Windows 10/11 x64
- Direct3D 11
- NVIDIA CUDA Toolkit 12.x

# Notes
- This repository uses `Core` as a submodule.
- Make sure submodules are initialized before building.
- CUDA Toolkit and NVIDIA Video Codec SDK must be installed and available in the build environment.

# Clone
```bash
git clone --recurse-submodules https://github.com/KommyButterCream/Core.git
```

If already cloned without submodules:
```bash
it submodule update --init --recursive
```
