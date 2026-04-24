#pragma once

class BitstreamRingBuffer;
class D3D11NvDecoder;

#ifdef BUILD_D3D11_NVIDIA_CODEC_DLL
#define DECODER_THREAD_API __declspec(dllexport)
#else
#define DECODER_THREAD_API __declspec(dllimport)
#endif

class DECODER_THREAD_API DecodeThread
{
public:
    DecodeThread();
    ~DecodeThread();

    bool Initialize(BitstreamRingBuffer* buffer, D3D11NvDecoder* decoder);
    void Shutdown();

private:

    static unsigned __stdcall ThreadProc(void* arg);
    void Run();

private:

    HANDLE m_thread = nullptr;
    unsigned m_threadId = 0;

    volatile LONG m_running = FALSE;

    BitstreamRingBuffer* m_bitstreamBuffer = nullptr;
    D3D11NvDecoder* m_decoder = nullptr;
};

