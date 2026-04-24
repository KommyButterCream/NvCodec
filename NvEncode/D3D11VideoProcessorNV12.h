#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11VideoDevice;
struct ID3D11VideoContext;
struct ID3D11VideoProcessorEnumerator;
struct ID3D11VideoProcessor;
struct ID3D11VideoProcessorInputView;
struct ID3D11VideoProcessorOutputView;

//D3D11VideoProcessorNV12 converter;
//
//converter.Initialize(
//    device,
//    context,
//    bgraRingTextures,
//    nv12RingTextures,
//    3,
//    width,
//    height);
//
//converter.Convert(currentIndex);

class D3D11VideoProcessorNV12
{
public:
    D3D11VideoProcessorNV12();
    ~D3D11VideoProcessorNV12();

    bool Initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        int32_t width,
        int32_t height);

    bool SetInputTextures(
        ID3D11Texture2D** textures,
        uint32_t bufferCount);

    bool SetOutputTextures(
        ID3D11Texture2D** textures,
        uint32_t bufferCount);

    bool Convert(uint32_t index);

    void Destory();


public:
    void SaveNV12ToFile(ID3D11Texture2D* NV12Texture, const char* fileName);
    void SaveNV12ToFile(uint32_t index, const char* fileName);

private:
    void ReleaseInputViews();
    void ReleaseOutputViews();
    void UpdateBufferCount();

private:
    ID3D11Device* m_D3D11Device = nullptr;
    ID3D11DeviceContext* m_D3D11Context = nullptr;

    ID3D11VideoDevice* m_videoDevice = nullptr;
    ID3D11VideoContext* m_videoContext = nullptr;

    ID3D11VideoProcessorEnumerator* m_enum = nullptr;
    ID3D11VideoProcessor* m_processor = nullptr;

    ID3D11VideoProcessorInputView** m_inputViews = nullptr;
    ID3D11VideoProcessorOutputView** m_outputViews = nullptr;

    uint32_t m_inputViewCount = 0;
    uint32_t m_outputViewCount = 0;

    uint32_t m_bufferCount = 0;
};

