#pragma once

// 仮想カメラの解像度。全カメラ共通で、インストール時に HKLM\SOFTWARE\iPhoneCam に書く。
// Frame Server 側のメディアソースはユーザーの設定を読めないため、HKLM に置く必要がある。
struct CameraConfig
{
    UINT32 width = 1920;
    UINT32 height = 1080;
};

CameraConfig LoadConfig();
