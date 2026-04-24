#include "pch.h"
#include "DecodeThread.h"
#include <process.h> // for _beginthreadex

#include "BitstreamRingBuffer.h"
#include "D3D11NvDecoder.h"

DecodeThread::DecodeThread()
{
}

DecodeThread::~DecodeThread()
{
    Shutdown();
}

bool DecodeThread::Initialize(BitstreamRingBuffer* buffer, D3D11NvDecoder* decoder)
{
    if (!buffer || !decoder)
        return false;

    m_bitstreamBuffer = buffer;
    m_decoder = decoder;

    ::InterlockedExchange(&m_running, TRUE);

    // 디코드 스레드 생성
    m_thread = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr,
        0,
        ThreadProc,
        this,
        0,
        &m_threadId
    ));

    if (!m_thread)
    {
        ::InterlockedExchange(&m_running, FALSE);
        return false;
    }

    return true;
}

void DecodeThread::Shutdown()
{
    if (::InterlockedExchange(&m_running, FALSE) == FALSE)
        return;

    if (m_bitstreamBuffer)
    {
        m_bitstreamBuffer->Shutdown();
    }

    // 스레드 완전 종료 대기
    if (m_thread)
    {
        ::WaitForSingleObject(m_thread, INFINITE);
        ::CloseHandle(m_thread);
        m_thread = nullptr;
    }
}

unsigned __stdcall DecodeThread::ThreadProc(void* arg)
{
    DecodeThread* self = reinterpret_cast<DecodeThread*>(arg);
    self->Run();
    return 0;
}

void DecodeThread::Run()
{
    while (::InterlockedCompareExchange(&m_running, TRUE, TRUE))
    {
        // Decode 를 수행 하기 위한 EncodedPacket 을 하나 가져온다.
        BitstreamRingBuffer::EncodedPacket* packet = m_bitstreamBuffer->AcquireReadPacket();
        if (!packet)
        {
            break;
        }

        if (packet->size > 0)
        {
            // Decoder 에 Decode 요청
            m_decoder->Parse(packet->data, static_cast<uint32_t>(packet->size));

            m_decoder->GetFrame();
        }

        // Decoding 이 완료된 후에 EncodedPacket Slot 을 Release 해준다.
        // 중복 또는 Multi-Decoding 방지.
        m_bitstreamBuffer->ReleaseReadPacket();
    }
}
