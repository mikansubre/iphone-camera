#pragma once

#include <windows.h>
#include <initguid.h>
#include <cguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <propvarutil.h>
#include <sddl.h>

#include <wrl/implements.h>
#include <wrl/module.h>

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

// ks.h は GUID_NULL をマクロで再定義するので、他の SDK ヘッダーより後に読む
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// {0EB3AA43-2700-45C6-A6D1-DE5C6A928B8D}
DEFINE_GUID(CLSID_IPhoneCamSource, 0x0eb3aa43, 0x2700, 0x45c6, 0xa6, 0xd1, 0xde, 0x5c, 0x6a, 0x92, 0x8b, 0x8d);

// Frame Server がアクティベーション時に渡してくる、呼び出し元アプリの PID (非公開属性)
DEFINE_GUID(MF_FRAMESERVER_CLIENTCONTEXT_CLIENTPID, 0x5f8d322e, 0x0fe4, 0x43e4, 0x9e, 0x50, 0xd8, 0x3e, 0xcd, 0x9f, 0xc2, 0xb8);

extern HMODULE g_module;

std::wstring GuidToString(REFGUID guid);
