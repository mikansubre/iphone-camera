#include "pch.h"
#include "Activator.h"
#include "Log.h"

using Microsoft::WRL::ComPtr;

HMODULE g_module;

std::wstring GuidToString(REFGUID guid)
{
    wchar_t text[40];
    StringFromGUID2(guid, text, ARRAYSIZE(text));
    return text;
}

namespace
{
    class ClassFactory
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                              IClassFactory>
    {
    public:
        STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override
        {
            RETURN_HR_IF_NULL(E_POINTER, object);
            *object = nullptr;
            RETURN_HR_IF(CLASS_E_NOAGGREGATION, outer != nullptr);
            ComPtr<Activator> activator;
            RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<Activator>(&activator));
            return activator.CopyTo(riid, object);
        }

        STDMETHODIMP LockServer(BOOL lock) override
        {
            auto& module = Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule();
            lock ? module.IncrementObjectCount() : module.DecrementObjectCount();
            return S_OK;
        }
    };

    std::wstring ClsidKey()
    {
        return L"SOFTWARE\\Classes\\CLSID\\" + GuidToString(CLSID_IPhoneCamSource);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule();
        wil::SetResultLoggingCallback([](const wil::FailureInfo& failure) noexcept {
            wchar_t text[2048];
            if (SUCCEEDED(wil::GetFailureLogString(text, ARRAYSIZE(text), failure)))
                Log(L"ERROR %s", text);
        });
    }
    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    return Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule().GetObjectCount() == 0 ? S_OK : S_FALSE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID clsid, _In_ REFIID riid, _Outptr_ LPVOID* object)
{
    RETURN_HR_IF_NULL(E_POINTER, object);
    *object = nullptr;
    RETURN_HR_IF(CLASS_E_CLASSNOTAVAILABLE, clsid != CLSID_IPhoneCamSource);
    return Microsoft::WRL::Make<ClassFactory>()->QueryInterface(riid, object);
}

// Frame Server は HKLM の登録しか見ないため、登録は管理者権限で行う (install_vcam.ps1)
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    RETURN_LAST_ERROR_IF(GetModuleFileNameW(g_module, path, MAX_PATH) == 0);

    wil::unique_hkey key;
    const auto keyPath = ClsidKey() + L"\\InprocServer32";
    RETURN_IF_WIN32_ERROR(RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                                          &key, nullptr));
    RETURN_IF_WIN32_ERROR(RegSetValueExW(key.get(), nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path),
                                         static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t))));
    constexpr wchar_t threading[] = L"Both";
    RETURN_IF_WIN32_ERROR(RegSetValueExW(key.get(), L"ThreadingModel", 0, REG_SZ,
                                         reinterpret_cast<const BYTE*>(threading), sizeof(threading)));
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKey().c_str());
    RETURN_HR_IF(HRESULT_FROM_WIN32(status), status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND);
    return S_OK;
}
