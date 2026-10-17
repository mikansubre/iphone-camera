#include "pch.h"
#include "Activator.h"
#include "MediaSource.h"
#include "Log.h"

namespace
{
    std::wstring GetDeviceInterfaceString(const std::wstring& symbolicLink, const DEVPROPKEY& key)
    {
        DEVPROPTYPE type = 0;
        ULONG size = 0;
        if (CM_Get_Device_Interface_PropertyW(symbolicLink.c_str(), &key, &type, nullptr, &size, 0) !=
                CR_BUFFER_SMALL ||
            type != DEVPROP_TYPE_STRING)
            return {};
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (CM_Get_Device_Interface_PropertyW(symbolicLink.c_str(), &key, &type,
                                              reinterpret_cast<PBYTE>(value.data()), &size, 0) != CR_SUCCESS)
            return {};
        value.resize(wcslen(value.c_str()));
        return value;
    }
}

HRESULT Activator::RuntimeClassInitialize()
{
    RETURN_IF_FAILED(MFCreateAttributes(&_attributes, 4));
    RETURN_IF_FAILED(_attributes->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1));
    RETURN_IF_FAILED(_attributes->SetGUID(MFT_TRANSFORM_CLSID_Attribute, CLSID_IPhoneCamSource));
    return S_OK;
}

std::wstring Activator::ResolveFriendlyName()
{
    // メディアソースには「どの仮想カメラとして開かれたか」が直接は渡されない。
    // Frame Server は MFCreateVirtualCamera に渡した名前そのものを属性で渡してくるので、それを使う。
    UINT32 length = 0;
    wil::unique_cotaskmem_string friendly;
    if (SUCCEEDED(_attributes->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly, &length)))
    {
        Log(L"Activator: friendly name attribute '%s'", friendly.get());
        return friendly.get();
    }

    // 予備: シンボリックリンクからデバイス名を引く。Windows が末尾に付ける " (Windows 仮想カメラ)" は外す
    wil::unique_cotaskmem_string link;
    if (SUCCEEDED(_attributes->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link,
                                                  &length)))
    {
        auto name = GetDeviceInterfaceString(link.get(), DEVPKEY_DeviceInterface_FriendlyName);
        if (const auto paren = name.rfind(L" ("); paren != std::wstring::npos && name.back() == L')')
            name.resize(paren);
        Log(L"Activator: symbolic link '%s' -> friendly name '%s'", link.get(), name.c_str());
        if (!name.empty())
            return name;
    }

    Log(L"Activator: could not identify the camera");
    return {};
}

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** object)
{
    RETURN_HR_IF_NULL(E_POINTER, object);
    *object = nullptr;
    std::lock_guard lock(_lock);

    if (!_source)
    {
        LogAttributes(_attributes.get(), L"Activator attr");
        const auto name = ResolveFriendlyName();
        Microsoft::WRL::ComPtr<MediaSource> source;
        RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaSource>(&source, _attributes.get(), name));
        RETURN_IF_FAILED(source.CopyTo(IID_PPV_ARGS(&_source)));
    }
    return _source->QueryInterface(riid, object);
}

STDMETHODIMP Activator::ShutdownObject()
{
    return S_OK;
}

STDMETHODIMP Activator::DetachObject()
{
    std::lock_guard lock(_lock);
    _source.reset();
    return S_OK;
}
