#pragma once

#include "../../Core/Concurrency/ThreadBase.h"

class D3D11NvEncoder_Impl;

class EncodeCompletionThread final : public Core::Concurrency::ThreadBase
{
public:
	EncodeCompletionThread();
	~EncodeCompletionThread();

	bool Initialize(D3D11NvEncoder_Impl* encoder);
	void Shutdown();

private:
	void Run() override;

private:
	D3D11NvEncoder_Impl* m_encoder = nullptr;
};
