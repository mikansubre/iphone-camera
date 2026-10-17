#pragma once

// C:\ProgramData\iPhoneCam\logs\<プロセス名>.log に追記する。
// DLL は Frame Server (session 0 のサービス) の中でも動くため、デバッガの代わりにファイルへ残す。
// ログフォルダが無ければ何もしない (インストーラが作る)。
void Log(_Printf_format_string_ const wchar_t* format, ...);
void LogAttributes(IMFAttributes* attributes, const wchar_t* prefix);
