#include "pch.h"
#include "Log.h"

#include <cstdarg>
#include <cwchar>

namespace
{
    std::mutex g_lock;

    std::wstring LogPath()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* name = wcsrchr(exe, L'\\');
        return std::wstring(L"C:\\ProgramData\\iPhoneCam\\logs\\") + (name ? name + 1 : exe) + L".log";
    }
}

void Log(const wchar_t* format, ...)
{
    wchar_t message[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[2200];
    int len = swprintf_s(line, L"%02u:%02u:%02u.%03u [%lu:%lu] %s\r\n", t.wHour, t.wMinute, t.wSecond,
                         t.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), message);
    if (len <= 0)
        return;

    char utf8[6600];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, len, utf8, sizeof(utf8), nullptr, nullptr);
    if (bytes <= 0)
        return;

    static const std::wstring path = LogPath();
    std::lock_guard lock(g_lock);
    wil::unique_hfile file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file)
    {
        DWORD written;
        WriteFile(file.get(), utf8, bytes, &written, nullptr);
    }
}

void LogAttributes(IMFAttributes* attributes, const wchar_t* prefix)
{
    UINT32 count = 0;
    if (!attributes || FAILED(attributes->GetCount(&count)))
        return;

    for (UINT32 i = 0; i < count; i++)
    {
        GUID key;
        PROPVARIANT value;
        PropVariantInit(&value);
        if (FAILED(attributes->GetItemByIndex(i, &key, &value)))
            continue;

        wchar_t text[512] = L"?";
        switch (value.vt)
        {
        case VT_UI4: swprintf_s(text, L"%u", value.ulVal); break;
        case VT_UI8: swprintf_s(text, L"%llu", value.uhVal.QuadPart); break;
        case VT_LPWSTR: swprintf_s(text, L"'%s'", value.pwszVal); break;
        case VT_CLSID: swprintf_s(text, L"%s", GuidToString(*value.puuid).c_str()); break;
        case VT_UNKNOWN: swprintf_s(text, L"IUnknown %p", value.punkVal); break;
        default: swprintf_s(text, L"vt=%u", value.vt); break;
        }
        Log(L"%s %s = %s", prefix, GuidToString(key).c_str(), text);
        PropVariantClear(&value);
    }
}
