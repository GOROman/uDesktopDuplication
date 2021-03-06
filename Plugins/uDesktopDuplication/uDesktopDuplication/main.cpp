#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <string>

#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"

#pragma comment(lib, "dxgi.lib")


namespace
{
    struct Monitor
    {
        int                     id            = -1;
        IDXGIOutputDuplication* deskDupl      = nullptr;
        ID3D11Texture2D*        texture       = nullptr;
        DXGI_OUTPUT_DESC        outputDesc;
        MONITORINFOEX           monitorInfo;

        struct Pointer
        {
            bool   isVisible        = false;
            int    x                = -1;
            int    y                = -1;
            BYTE*  apiBuffer        = nullptr;
            UINT   apiBufferSize    = 0;
            BYTE*  bgra32Buffer     = nullptr;
            UINT   bgra32BufferSize = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
        };
        Pointer pointer;
    };

    IUnityInterfaces*    g_unity        = nullptr;
    int                  g_mouseMonitor = 0;
    int                  g_timeout      = 10;
    HRESULT              g_errorCode    = 0;
    std::string          g_errorMessage = "";
    std::vector<Monitor> g_monitors;
}


extern "C"
{
    void FinalizeDuplication()
    {
        for (auto& monitor : g_monitors)
        {
            monitor.deskDupl->Release();
        }
        g_monitors.clear();
    }

    void InitializeDuplication()
    {
        FinalizeDuplication();

        IDXGIFactory1* factory;
        CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));

        // Check all display adapters.
        int id = 0;
        IDXGIAdapter1* adapter;
        for (int i = 0; (factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND); ++i) 
        {
            // Search the main monitor from all outputs.
            IDXGIOutput* output;
            for (int j = 0; (adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND); ++j) 
            {
                Monitor monitor;
                monitor.id = id++;

                output->GetDesc(&monitor.outputDesc);
                monitor.monitorInfo.cbSize = sizeof(MONITORINFOEX);
                GetMonitorInfo(monitor.outputDesc.Monitor, &monitor.monitorInfo);

                auto device = g_unity->Get<IUnityGraphicsD3D11>()->GetDevice();
                auto output1 = reinterpret_cast<IDXGIOutput1*>(output);
                output1->DuplicateOutput(device, &monitor.deskDupl);
                output->Release();

                g_monitors.push_back(monitor);
            }

            adapter->Release();
        }

        factory->Release();
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
    {
        g_unity = unityInterfaces;
        InitializeDuplication();
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload()
    {
        g_unity = nullptr;
        FinalizeDuplication();
    }

    bool IsValidMonitorId(int id)
    {
        return id >= 0 && id < g_monitors.size();
    }

    bool UpdateMouse(const DXGI_OUTDUPL_FRAME_INFO& frameInfo, Monitor& monitor)
    {
        auto& pointer = monitor.pointer;
        pointer.isVisible = frameInfo.PointerPosition.Visible != 0;
        pointer.x = frameInfo.PointerPosition.Position.x;
        pointer.y = frameInfo.PointerPosition.Position.y;

        // Pointer type
        const auto pointerType = pointer.shapeInfo.Type;
        const bool isMono = pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        const bool isColorMask = pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;

        if (pointer.isVisible)
        {
            g_mouseMonitor = monitor.id;
        }

        if (g_mouseMonitor != monitor.id) {
            return true;
        }

        // Increase the buffer size if needed
        if (frameInfo.PointerShapeBufferSize > pointer.apiBufferSize)
        {
            if (pointer.apiBuffer) delete[] pointer.apiBuffer;
            pointer.apiBuffer = new BYTE[frameInfo.PointerShapeBufferSize];
            pointer.apiBufferSize = frameInfo.PointerShapeBufferSize;
        }
        if (!pointer.apiBuffer) return true;

        // Get information about the mouse pointer if needed
        if (frameInfo.PointerShapeBufferSize != 0)
        {
            UINT bufferSize;
            monitor.deskDupl->GetFramePointerShape(
                frameInfo.PointerShapeBufferSize,
                reinterpret_cast<void*>(pointer.apiBuffer),
                &bufferSize,
                &pointer.shapeInfo);
        }

        // Size
        const auto w = pointer.shapeInfo.Width;
        const auto h = pointer.shapeInfo.Height / (isMono ? 2 : 1);
        const auto p = pointer.shapeInfo.Pitch; 

        // Convert the buffer given by API into BGRA32
        const auto bgraBufferSize = w * h * 4;
        if (bgraBufferSize > pointer.bgra32BufferSize)
        {
            if (pointer.bgra32Buffer) delete[] pointer.bgra32Buffer;
            pointer.bgra32Buffer = new BYTE[bgraBufferSize];
            pointer.bgra32BufferSize = bgraBufferSize;
        }
        if (!pointer.bgra32Buffer) return true;

        // If masked, copy the desktop image and merge it with masked image.
        if (isMono || isColorMask)
        {
            HRESULT hr;

            D3D11_TEXTURE2D_DESC desc;
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;

            ID3D11DeviceContext* context;
            auto device = g_unity->Get<IUnityGraphicsD3D11>()->GetDevice();
            device->GetImmediateContext(&context);

            ID3D11Texture2D* texture = nullptr;
            hr = device->CreateTexture2D(&desc, nullptr, &texture);
            if (FAILED(hr)) 
            {
                return false;
            }

            D3D11_BOX box;
            box.front = 0;
            box.back = 1;
            box.left = pointer.x;
            box.top = pointer.y;
            box.right = pointer.x + w;
            box.bottom = pointer.y + h;
            context->CopySubresourceRegion(texture, 0, 0, 0, 0, monitor.texture, 0, &box);

            IDXGISurface* surface = nullptr;
            hr = texture->QueryInterface(__uuidof(IDXGISurface), (void**)&surface);
            texture->Release();
            if (FAILED(hr))
            {
                return false;
            }

            DXGI_MAPPED_RECT mappedSurface;
            hr = surface->Map(&mappedSurface, DXGI_MAP_READ);
            if (FAILED(hr))
            {
                surface->Release();
                return false;
            }

            // Finally, get the texture behind the mouse pointer.
            const auto desktop32 = reinterpret_cast<UINT*>(mappedSurface.pBits);
            const UINT desktopPitch = mappedSurface.Pitch / sizeof(UINT);

            // Access RGBA values at the same time
            auto output32 = reinterpret_cast<UINT*>(pointer.bgra32Buffer);

            if (isMono)
            {
                for (UINT row = 0; row < h; ++row) 
                {
                    BYTE mask = 0x80;
                    for (UINT col = 0; col < w; ++col) 
                    {
                        const int i = row * w + col;
                        const BYTE andMask = pointer.apiBuffer[col / 8 + row * p] & mask;
                        const BYTE xorMask = pointer.apiBuffer[col / 8 + (row + h) * p] & mask;
                        const UINT andMask32 = andMask ? 0xFFFFFFFF : 0xFF000000;
                        const UINT xorMask32 = xorMask ? 0x00FFFFFF : 0x00000000;
                        output32[i] = (desktop32[row * desktopPitch + col] & andMask32) ^ xorMask32;
                        mask = (mask == 0x01) ? 0x80 : (mask >> 1);
                    }
                }
            }
            else // DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR
            {
                const auto buffer32 = reinterpret_cast<UINT*>(pointer.apiBuffer);

                for (UINT row = 0; row < h; ++row) 
                {
                    for (UINT col = 0; col < w; ++col) 
                    {
                        const int i = row * w + col;
                        const int j = row * p / sizeof(UINT) + col;

                        UINT mask = 0xFF000000 & buffer32[j];
                        if (mask)
                        {
                            output32[i] = (desktop32[row * desktopPitch + col] ^ buffer32[j]) | 0xFF000000;
                        }
                        else
                        {
                            output32[i] = buffer32[j] | 0xFF000000;
                        }
                    }
                }
            }

            hr = surface->Unmap();
            surface->Release();
            if (FAILED(hr))
            {
                return false;
            }
        }
        else // DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR
        {
            auto output32 = reinterpret_cast<UINT*>(pointer.bgra32Buffer);
            const auto buffer32 = reinterpret_cast<UINT*>(pointer.apiBuffer);
            for (UINT i = 0; i < w * h; ++i) 
            {
                output32[i] = buffer32[i];
            }
        }

        return true;
    }

    void UNITY_INTERFACE_API OnRenderEvent(int id)
    {
        if (!IsValidMonitorId(id)) return;
        auto& monitor = g_monitors[id];

        if (monitor.deskDupl == nullptr || monitor.texture == nullptr) return;

        IDXGIResource* resource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;

        g_errorCode = monitor.deskDupl->AcquireNextFrame(g_timeout, &frameInfo, &resource);
        if (FAILED(g_errorCode))
        {
            if (g_errorCode == DXGI_ERROR_ACCESS_LOST)
            {
                InitializeDuplication();
                g_errorMessage = "[IDXGIOutputDuplication::AcquireNextFrame()] Access lost.";
            }
            else
            {
                g_errorMessage = "[IDXGIOutputDuplication::AcquireNextFrame()] Maybe timeout.";
            }
            return;
        }

        ID3D11Texture2D* texture;
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));

        ID3D11DeviceContext* context;
        auto device = g_unity->Get<IUnityGraphicsD3D11>()->GetDevice();
        device->GetImmediateContext(&context);
        context->CopyResource(monitor.texture, texture);

        if (!UpdateMouse(frameInfo, monitor))
        {
            g_errorCode = -999;
            g_errorMessage = "[UpdateMouse()] failed.";
        }

        resource->Release();
        monitor.deskDupl->ReleaseFrame();
    }

    UNITY_INTERFACE_EXPORT UnityRenderingEvent UNITY_INTERFACE_API GetRenderEventFunc()
    {
        return OnRenderEvent;
    }

    UNITY_INTERFACE_EXPORT size_t UNITY_INTERFACE_API GetMonitorCount()
    {
        return g_monitors.size();
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API SetTimeout(int timeout)
    {
        g_timeout = timeout;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API GetName(int id, char* buf, int len)
    {
        if (!IsValidMonitorId(id)) return;
        strcpy_s(buf, len, g_monitors[id].monitorInfo.szDevice);
    }

    UNITY_INTERFACE_EXPORT bool UNITY_INTERFACE_API IsPrimary(int id)
    {
        if (!IsValidMonitorId(id)) return false;
        return g_monitors[id].monitorInfo.dwFlags == MONITORINFOF_PRIMARY;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GetWidth(int id)
    {
        if (!IsValidMonitorId(id)) return -1;
        const auto rect = g_monitors[id].monitorInfo.rcMonitor;
        return rect.right - rect.left;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GetHeight(int id)
    {
        if (!IsValidMonitorId(id)) return -1;
        const auto rect = g_monitors[id].monitorInfo.rcMonitor;
        return rect.bottom - rect.top;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API IsPointerVisible(int id)
    {
        if (!IsValidMonitorId(id)) return false;
        return g_monitors[id].pointer.isVisible;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GetPointerX(int id)
    {
        if (!IsValidMonitorId(id)) return -1;
        return g_monitors[id].pointer.x;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GetPointerY(int id)
    {
        if (!IsValidMonitorId(id)) return -1;
        return g_monitors[id].pointer.y;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GetPointerShapeWidth(int id)
    {
        if (!IsValidMonitorId(id)) return -1;
        return g_monitors[id].pointer.shapeInfo.Width;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GetPointerShapeHeight(int id)
    {
        if (!IsValidMonitorId(id)) return -1;
        const auto& info = g_monitors[id].pointer.shapeInfo;
        return (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) ? info.Height / 2 : info.Height;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GetPointerShapePitch(int id)
    {
        if (!IsValidMonitorId(id)) return -1;
        return g_monitors[id].pointer.shapeInfo.Pitch;
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GetPointerShapeType(int id)
    {
        if (!IsValidMonitorId(id)) return -1;
        return g_monitors[id].pointer.shapeInfo.Type;
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UpdatePointerTexture(int id, ID3D11Texture2D* ptr)
    {
        if (!IsValidMonitorId(id)) return;
        const auto& pointer = g_monitors[id].pointer;
        if (!pointer.bgra32Buffer) return;
        auto device = g_unity->Get<IUnityGraphicsD3D11>()->GetDevice();
        ID3D11DeviceContext* context;
        device->GetImmediateContext(&context);
        context->UpdateSubresource(ptr, 0, nullptr, pointer.bgra32Buffer, pointer.shapeInfo.Width * 4, 0);
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API SetTexturePtr(int id, void* texture)
    {
        if (!IsValidMonitorId(id)) return;
        g_monitors[id].texture = reinterpret_cast<ID3D11Texture2D*>(texture);
    }

    UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GetErrorCode()
    {
        const auto code = g_errorCode;
        g_errorCode = 0;
        return static_cast<int>(code);
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API GetErrorMessage(char* buf, int len)
    {
        const auto message = g_errorMessage;
        g_errorMessage = "";
        strcpy_s(buf, len, message.c_str());
    }

}