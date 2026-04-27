#pragma once

#include "../../Core/Concurrency/ThreadBase.h"

class BitstreamRingBuffer;
class D3D11NvDecoder;

class DecodeThread : public Core::Concurrency::ThreadBase
{
public:
	DecodeThread();
	~DecodeThread();

	bool Initialize(BitstreamRingBuffer* buffer, D3D11NvDecoder* decoder);
	void Shutdown();

private:
	void Run() override;

private:
	BitstreamRingBuffer* m_bitstreamBuffer = nullptr;
	D3D11NvDecoder* m_decoder = nullptr;
};
