# NvCodec
D3D11-based NvCodec DLL library

# Info
A D3D11-based DLL library for NVIDIA Video Codec SDK integration.
Provides H.264 encode/decode functionality built on top of Direct3D 11, CUDA, and the NVIDIA Video Codec SDK.

# Dependencies
- [Core](../Core)
- NVIDIA Video Codec SDK (v13.0.37)
- NVIDIA CUDA Toolkit (v12.8)

# Build Environment
- C++20
- MSVC (Visual Studio 2022)
- Windows 10/11 x64
- Direct3D 11
- NVIDIA CUDA Toolkit 12.8

# Repository Layout
This project expects `NvCodec` and `Core` to be placed under the same parent directory.

Example:
```text
Module/
├─ Core/
└─ NvCodec/
