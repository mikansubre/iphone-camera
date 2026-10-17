// server.py から ctypes で呼ぶ関数。仮想カメラの作成・削除と、フレームの書き込みを行う。
// カメラは MFVirtualCameraLifetime_Session なので、呼び出したプロセスが終われば自動で消える。

#include "pch.h"
#include "Config.h"
#include "FrameChannel.h"
#include "Log.h"

namespace
{
    struct Camera
    {
        std::mutex lock;
        wil::com_ptr_nothrow<IMFVirtualCamera> vcam;
        ChannelWriter writer;
    };

    HRESULT EnsureInitialized()
    {
        // どのスレッドから呼ばれても MTA で動かす
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        RETURN_HR_IF(hr, FAILED(hr) && hr != RPC_E_CHANGED_MODE);
        static const HRESULT started = MFStartup(MF_VERSION);
        return started;
    }
}

extern "C" HRESULT __stdcall ICam_GetFormat(UINT32* width, UINT32* height)
{
    RETURN_HR_IF_NULL(E_POINTER, width);
    RETURN_HR_IF_NULL(E_POINTER, height);
    const auto config = LoadConfig();
    *width = config.width;
    *height = config.height;
    return S_OK;
}

extern "C" HRESULT __stdcall ICam_Create(LPCWSTR friendlyName, void** handle)
{
    RETURN_HR_IF_NULL(E_POINTER, friendlyName);
    RETURN_HR_IF_NULL(E_POINTER, handle);
    *handle = nullptr;
    RETURN_IF_FAILED(EnsureInitialized());

    auto camera = std::make_unique<Camera>();
    RETURN_IF_FAILED_MSG(MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                                               MFVirtualCameraAccess_CurrentUser, friendlyName,
                                               GuidToString(CLSID_IPhoneCamSource).c_str(), nullptr, 0,
                                               &camera->vcam),
                         "MFCreateVirtualCamera '%ls'", friendlyName);
    RETURN_IF_FAILED_MSG(camera->vcam->Start(nullptr), "IMFVirtualCamera::Start '%ls'", friendlyName);

    const auto config = LoadConfig();
    camera->writer.Init(ChannelName(friendlyName), config.width, config.height);
    Log(L"Created virtual camera '%s'", friendlyName);
    *handle = camera.release();
    return S_OK;
}

extern "C" HRESULT __stdcall ICam_Send(void* handle, const BYTE* nv12, UINT32 size)
{
    RETURN_HR_IF_NULL(E_POINTER, handle);
    RETURN_HR_IF_NULL(E_POINTER, nv12);
    auto camera = static_cast<Camera*>(handle);
    std::lock_guard lock(camera->lock);
    return camera->writer.Write(nv12, size);
}

extern "C" BOOL __stdcall ICam_InUse(void* handle)
{
    if (!handle)
        return FALSE;
    auto camera = static_cast<Camera*>(handle);
    std::lock_guard lock(camera->lock);
    return camera->writer.InUse();
}

extern "C" HRESULT __stdcall ICam_Remove(void* handle)
{
    RETURN_HR_IF_NULL(E_POINTER, handle);
    RETURN_IF_FAILED(EnsureInitialized());
    std::unique_ptr<Camera> camera(static_cast<Camera*>(handle));
    // Shutdown は呼ばない (メディアソースに Shutdown が 2 回届いて削除に失敗するため)
    const HRESULT hr = camera->vcam->Remove();
    Log(L"Removed virtual camera hr=0x%08X", hr);
    return hr;
}
