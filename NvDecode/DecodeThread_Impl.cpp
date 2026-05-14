#include "pch.h"
#include "DecodeThread_Impl.h"

#include "BitstreamRingBuffer.h"

DecodeThread_Impl::DecodeThread_Impl()
	: Core::Concurrency::ThreadBase(L"DecodeThread")
{
}

DecodeThread_Impl::~DecodeThread_Impl()
{
	Shutdown();
}

bool DecodeThread_Impl::Initialize(BitstreamRingBuffer* buffer, D3D11NvDecoder* decoder)
{
	if (!buffer || !decoder)
		return false;

	Shutdown();

	m_bitstreamBuffer = buffer;
	m_decoder = decoder;

	if (!Start())
	{
		m_bitstreamBuffer = nullptr;
		m_decoder = nullptr;
		return false;
	}

	return true;
}

void DecodeThread_Impl::Shutdown()
{
	if (m_bitstreamBuffer)
	{
		m_bitstreamBuffer->Shutdown();
	}

	Stop();

	m_bitstreamBuffer = nullptr;
	m_decoder = nullptr;
}

void DecodeThread_Impl::SetFrameCallback(FrameCallback callback, void* userData)
{
	m_frameCallback = callback;
	m_frameCallbackUserData = userData;
}

void DecodeThread_Impl::Run()
{
	while (!IsStopRequested())
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
			if (m_decoder->Parse(packet->data, static_cast<uint32_t>(packet->size)))
			{
				while (D3D11NvDecoder::Frame* frame = m_decoder->GetFrame())
				{
					if (m_frameCallback)
					{
						m_frameCallback(*frame, m_frameCallbackUserData);
					}
				}
			}
		}

		// Decoding 이 완료된 후에 EncodedPacket Slot 을 Release 해준다.
		// 중복 또는 Multi-Decoding 방지.
		m_bitstreamBuffer->ReleaseReadPacket();
	}
}
