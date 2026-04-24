#include "pch.h"
#include "D3D11VideoProcessorNV12.h"
#include <stdio.h> // for printf_s, fopen_s, fwrite

D3D11VideoProcessorNV12::D3D11VideoProcessorNV12()
{
}

D3D11VideoProcessorNV12::~D3D11VideoProcessorNV12()
{
	Destory();
}

bool D3D11VideoProcessorNV12::Initialize(
	ID3D11Device* device,
	ID3D11DeviceContext* context,
	int32_t width,
	int32_t height)
{
	// D3D11Texture BGRA 를 NV12 로 변환하는 DeD11VideoProcessor 를 초기화 한다.

	if (!device || !context)
		return false;

	Destory();

	// 외부에서 받은 D3D11Device 와 D3D11DeviceContext 를 사용한다.
	// 레퍼런스 카운트 증가. Shutdown 에서 Release 호출 필수
	m_D3D11Device = device;
	m_D3D11Context = context;

	m_D3D11Device->AddRef();
	m_D3D11Context->AddRef();

	HRESULT hr = S_OK;

	// D3D11 Video Device 획득
	hr = m_D3D11Device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&m_videoDevice));
	if (FAILED(hr))
	{
		Destory();
		return false;
	}


	// D3D11 Video Context 획득
	hr = m_D3D11Context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&m_videoContext));
	if (FAILED(hr))
	{
		Destory();
		return false;
	}

	// 변환을 위한 Processor Context Desc 를 설정
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
	desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	desc.InputFrameRate = { 1, 1 };
	desc.InputWidth = width;
	desc.InputHeight = height;
	desc.OutputFrameRate = { 1, 1 };
	desc.OutputWidth = width;
	desc.OutputHeight = height;
	desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

	// Video Processor 에 대한 열거형 포인터를 생성
	hr = m_videoDevice->CreateVideoProcessorEnumerator(&desc, &m_enum);
	if (FAILED(hr))
	{
		Destory();
		return false;
	}

	// Video Processor 생성
	hr = m_videoDevice->CreateVideoProcessor(m_enum, 0, &m_processor);
	if (FAILED(hr))
	{
		Destory();
		return false;
	}

	// 색공간 설정
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE colorSpace = {};
	colorSpace.Usage = 1;          // 0 : 재생, 1 : 비디오 처리
	colorSpace.RGB_Range = 0;      // 0 : 전체 범위(0~255), 1 : 제한된 범위(16-235)
	colorSpace.YCbCr_Matrix = 1;   // 0 : ITU-R BT.601, 1 : ITU-R BT.709
	colorSpace.YCbCr_xvYCC = 0;    // 0 : 기존 YCbCr, 1 : 확장된 YCbCr(xvYCC)
	colorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;

	m_videoContext->VideoProcessorSetStreamColorSpace(m_processor, 0, &colorSpace);
	m_videoContext->VideoProcessorSetOutputColorSpace(m_processor, &colorSpace);

	// Input, Output 에서 처리될 프레임의 ROI 를 설정
	RECT rect = { 0, 0, (LONG)width, (LONG)height };
	m_videoContext->VideoProcessorSetStreamSourceRect(m_processor, 0, TRUE, &rect);
	m_videoContext->VideoProcessorSetStreamDestRect(m_processor, 0, TRUE, &rect);
	m_videoContext->VideoProcessorSetOutputTargetRect(m_processor, TRUE, &rect);

	return true;
}

bool D3D11VideoProcessorNV12::SetInputTextures(ID3D11Texture2D** textures, uint32_t bufferCount)
{
	// B8G8R8A8 타입의 D3D11Texture 를 받아서 VideoProcessor 의 Input 으로 설정한다.
	// BGRA->NV12 변환 전에 사용할 버퍼를 미리 설정 후 재사용 한다.

	ReleaseInputViews();

	if (!textures || bufferCount == 0)
	{
		UpdateBufferCount();
		return false;
	}

	// D3D11Texture 버퍼에 대응하는 InputView 생성
	m_inputViews = new ID3D11VideoProcessorInputView * [bufferCount] {};
	m_inputViewCount = bufferCount;

	for (uint32_t i = 0; i < bufferCount; i++)
	{
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc = {};
		inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		inDesc.Texture2D.MipSlice = 0;
		inDesc.Texture2D.ArraySlice = 0;

		HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(
			textures[i],
			m_enum,
			&inDesc,
			&m_inputViews[i]);

		if (FAILED(hr))
		{
			ReleaseInputViews();
			UpdateBufferCount();
			return false;
		}
	}

	UpdateBufferCount();

	return true;
}

bool D3D11VideoProcessorNV12::SetOutputTextures(ID3D11Texture2D** textures, uint32_t bufferCount)
{
	// NV12 타입의 D3D11Texture 를 받아서 VideoProcessor 의 Output 으로 설정한다.
	// BGRA->NV12 변환 전에 사용할 버퍼를 미리 설정 후 재사용 한다.

	ReleaseOutputViews();

	if (!textures || bufferCount == 0)
	{
		UpdateBufferCount();
		return false;
	}

	// D3D11Texture 버퍼에 대응하는 OutputView 생성
	m_outputViews = new ID3D11VideoProcessorOutputView * [bufferCount] {};
	m_outputViewCount = bufferCount;

	for (uint32_t i = 0; i < bufferCount; i++)
	{
		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc = {};
		outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		outDesc.Texture2D.MipSlice = 0;

		HRESULT hr = m_videoDevice->CreateVideoProcessorOutputView(
			textures[i],
			m_enum,
			&outDesc,
			&m_outputViews[i]);

		if (FAILED(hr))
		{
			ReleaseOutputViews();
			UpdateBufferCount();
			return false;
		}
	}

	UpdateBufferCount();

	return true;
}

bool D3D11VideoProcessorNV12::Convert(uint32_t index)
{
	// BGRA -> NV12 변환 수행
	// 사용자로 부터 입력받은 Index 의 Input View 를 Output View 에 변환 한다.

	if (!m_videoContext || !m_processor || !m_inputViews || !m_outputViews)
		return false;

	const uint32_t availableBufferCount =
		(m_inputViewCount < m_outputViewCount) ? m_inputViewCount : m_outputViewCount;

	if (availableBufferCount == 0 || index >= availableBufferCount)
		return false;

	if (!m_inputViews[index] || !m_outputViews[index])
		return false;

	D3D11_VIDEO_PROCESSOR_STREAM stream = {};
	stream.Enable = TRUE;
	stream.pInputSurface = m_inputViews[index];

	HRESULT hr = m_videoContext->VideoProcessorBlt(
		m_processor,
		m_outputViews[index],
		0,
		1,
		&stream
	);

	return SUCCEEDED(hr);
}

void D3D11VideoProcessorNV12::Destory()
{
	// Video Processor 에서 사용된 리소스 해제

	ReleaseInputViews();
	ReleaseOutputViews();
	m_bufferCount = 0;

	SafeRelease(m_processor);
	SafeRelease(m_enum);
	SafeRelease(m_videoContext);
	SafeRelease(m_videoDevice);
	SafeRelease(m_D3D11Context);
	SafeRelease(m_D3D11Device);
}

void D3D11VideoProcessorNV12::ReleaseInputViews()
{
	if (!m_inputViews)
	{
		m_inputViewCount = 0;
		return;
	}

	for (uint32_t i = 0; i < m_inputViewCount; i++)
	{
		SafeRelease(m_inputViews[i]);
	}

	delete[] m_inputViews;
	m_inputViews = nullptr;
	m_inputViewCount = 0;
}

void D3D11VideoProcessorNV12::ReleaseOutputViews()
{
	if (!m_outputViews)
	{
		m_outputViewCount = 0;
		return;
	}

	for (uint32_t i = 0; i < m_outputViewCount; i++)
	{
		SafeRelease(m_outputViews[i]);
	}

	delete[] m_outputViews;
	m_outputViews = nullptr;
	m_outputViewCount = 0;
}

void D3D11VideoProcessorNV12::UpdateBufferCount()
{
	if (m_inputViewCount == 0 || m_outputViewCount == 0)
	{
		m_bufferCount = 0;
		return;
	}

	m_bufferCount = (m_inputViewCount < m_outputViewCount) ? m_inputViewCount : m_outputViewCount;
}

void D3D11VideoProcessorNV12::SaveNV12ToFile(ID3D11Texture2D* NV12Texture, const char* fileName)
{
	// BGRA -> NV12 변환 이 잘 되었는지 스테이징 버퍼로 복사하여 파일로 Write 한다.

	D3D11_TEXTURE2D_DESC desc = {};
	NV12Texture->GetDesc(&desc);

	// Staging 텍스처 생성
	D3D11_TEXTURE2D_DESC stagingDesc = desc;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.BindFlags = 0;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.MiscFlags = 0;

	ID3D11Texture2D* stagingTexture = nullptr;
	HRESULT hr = m_D3D11Device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);

	if (FAILED(hr))
		return;

	m_D3D11Context->CopyResource(stagingTexture, NV12Texture);

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(m_D3D11Context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		// 실제 시스템의 Pitch 출력 (여기서 2048 등이 찍힐 겁니다)
		printf("[Debug] Width: %u, Height: %u, Actual Pitch: %u\n", desc.Width, desc.Height, mapped.RowPitch);

		FILE* fp = nullptr;
		fopen_s(&fp, fileName, "wb");
		if (fp)
		{
			size_t fileSize = 0;
			uint8_t* pSrc = (uint8_t*)mapped.pData;

			// 1. Y Plane 저장
			for (uint32_t i = 0; i < desc.Height; i++)
			{
				fwrite(pSrc + (i * mapped.RowPitch), 1, desc.Width, fp);
			}
			fileSize += (desc.Width * desc.Height);

			// 2. UV Plane 시작 지점
			uint8_t* pUVStart = pSrc + (desc.Height * mapped.RowPitch);

			// 3. UV Plane 저장
			for (uint32_t i = 0; i < desc.Height / 2; i++)
			{
				fwrite(pUVStart + (i * mapped.RowPitch), 1, desc.Width, fp);
			}

			fileSize += (desc.Width * (desc.Height / 2));

			fclose(fp);
			printf_s("[Success] File saved. Expected size: %lld bytes.\n", fileSize);
		}
		m_D3D11Context->Unmap(stagingTexture, 0);
	}

	SafeRelease(stagingTexture);
}

void D3D11VideoProcessorNV12::SaveNV12ToFile(uint32_t index, const char* fileName)
{
	// BGRA -> NV12 변환 이 잘 되었는지 스테이징 버퍼로 복사하여 파일로 Write 한다.

	if (!m_outputViews)
		return;

	if (m_outputViewCount <= index)
		return;

	ID3D11VideoProcessorOutputView* outputView = m_outputViews[index];

	if (!outputView)
		return;

	ID3D11Resource* resource = nullptr;
	outputView->GetResource(&resource);
	if (!resource)
		return;

	ID3D11Texture2D* outputTexture = static_cast<ID3D11Texture2D*>(resource);

	SaveNV12ToFile(outputTexture, fileName);

	SafeRelease(resource);
}
