#include "pch.h"
#include "Config.h"

CameraConfig LoadConfig()
{
    CameraConfig config;
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\iPhoneCam", L"Width", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) == ERROR_SUCCESS &&
        value >= 160 && value <= 7680 && value % 2 == 0)
        config.width = value;
    size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\iPhoneCam", L"Height", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) == ERROR_SUCCESS &&
        value >= 120 && value <= 4320 && value % 2 == 0)
        config.height = value;
    return config;
}
