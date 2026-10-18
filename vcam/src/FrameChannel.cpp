#include "pch.h"
#include "FrameChannel.h"
#include "Log.h"

namespace
{
    // SYSTEM / LocalService / Administrators / 対話ログオンのユーザーにだけ許可する
    constexpr wchar_t kChannelSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;IU)";

    UINT64 Fnv1a(const std::wstring& text)
    {
        UINT64 hash = 0xcbf29ce484222325ull;
        for (wchar_t c : text)
        {
            hash ^= static_cast<UINT64>(towlower(c));
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    BYTE* SlotPtr(BYTE* view, const ChannelHeader* header, LONG slot)
    {
        return view + kChannelHeaderSize + static_cast<size_t>(slot) * header->frameSize;
    }
}

std::wstring ChannelName(const std::wstring& friendlyName)
{
    wchar_t name[64];
    swprintf_s(name, L"Global\\iPhoneCam.%016llx", Fnv1a(friendlyName));
    return name;
}

UINT32 Nv12FrameSize(UINT32 width, UINT32 height)
{
    return width * height * 3 / 2;
}

// ---- Reader (Frame Server 側) ----

HRESULT ChannelReader::Create(const std::wstring& name, UINT32 width, UINT32 height)
{
    Close();
    wil::unique_hlocal_security_descriptor sd;
    RETURN_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        kChannelSddl, SDDL_REVISION_1, wil::out_param_ptr<PSECURITY_DESCRIPTOR*>(sd), nullptr));
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd.get(), FALSE};

    const UINT32 frameSize = Nv12FrameSize(width, height);
    const UINT64 total = kChannelHeaderSize + static_cast<UINT64>(frameSize) * kChannelSlots;
    _mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, static_cast<DWORD>(total >> 32),
                                      static_cast<DWORD>(total), (name + L".map").c_str()));
    RETURN_LAST_ERROR_IF_NULL_MSG(_mapping.get(), "CreateFileMapping %ls", name.c_str());
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;

    _event.reset(CreateEventW(&sa, FALSE, FALSE, (name + L".evt").c_str()));
    RETURN_LAST_ERROR_IF_NULL(_event.get());

    _view.reset(static_cast<BYTE*>(MapViewOfFile(_mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, 0)));
    RETURN_LAST_ERROR_IF_NULL(_view.get());
    _header = reinterpret_cast<ChannelHeader*>(_view.get());

    if (!existed || _header->magic != kChannelMagic)
    {
        _header->width = width;
        _header->height = height;
        _header->frameSize = frameSize;
        _header->slots = kChannelSlots;
        _header->seq = 0;
        _header->latest = 0;
        _header->version = kChannelVersion;
        MemoryBarrier();
        _header->magic = kChannelMagic;
    }
    else if (_header->width != width || _header->height != height)
    {
        Log(L"Channel %s size mismatch %ux%u (expected %ux%u)", name.c_str(), _header->width, _header->height, width,
            height);
        Close();
        return E_UNEXPECTED;
    }
    Log(L"Channel %s ready (%s)", name.c_str(), existed ? L"writer already waiting" : L"new");
    return S_OK;
}

void ChannelReader::Close()
{
    _header = nullptr;
    _view.reset();
    _event.reset();
    _mapping.reset();
}

bool ChannelReader::CopyLatest(BYTE* dst, LONG pitch, LONG64* seq)
{
    if (!_header)
        return false;
    const UINT32 w = _header->width, h = _header->height;

    // seqlock: Writer は最新の次のスロットへ書くので、コピー中のスロットが上書きされるのは、
    // コピーの間に seq が kChannelSlots - 1 以上進んだときだけ。そうなっていたら読み直す
    for (int attempt = 0;; attempt++)
    {
        const LONG64 before = _header->seq;
        if (before == 0)
            return false;
        MemoryBarrier();
        const BYTE* src = SlotPtr(_view.get(), _header, _header->latest);
        for (UINT32 y = 0; y < h + h / 2; y++)  // Y 面と UV 面は同じ行間隔で連続している
            memcpy(dst + static_cast<size_t>(y) * pitch, src + static_cast<size_t>(y) * w, w);
        MemoryBarrier();
        *seq = before;
        if (_header->seq - before < static_cast<LONG64>(kChannelSlots) - 1)
            return true;
        if (attempt == 3)
        {
            // 黒を出すよりは、崩れているかもしれない 1 枚を出すほうがまし
            Log(L"CopyLatest: writer kept overtaking the reader");
            return true;
        }
    }
}

void ChannelReader::Heartbeat()
{
    if (_header)
        InterlockedExchange64(&_header->readerHeartbeat, static_cast<LONG64>(GetTickCount64()));
}

// ---- Writer (server.py のプロセス側) ----

void ChannelWriter::Init(const std::wstring& name, UINT32 width, UINT32 height)
{
    _name = name;
    _width = width;
    _height = height;
}

bool ChannelWriter::TryOpen()
{
    if (_header)
        return true;
    const ULONGLONG now = GetTickCount64();
    if (now < _nextOpenTry)
        return false;
    _nextOpenTry = now + 250;  // 使われていない間は 4 回/秒だけ確認する

    _mapping.reset(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, (_name + L".map").c_str()));
    if (!_mapping)
        return false;
    _event.reset(OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (_name + L".evt").c_str()));
    _view.reset(static_cast<BYTE*>(MapViewOfFile(_mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0)));
    if (!_event || !_view)
    {
        Close();
        return false;
    }

    auto header = reinterpret_cast<ChannelHeader*>(_view.get());
    if (header->magic != kChannelMagic || header->width != _width || header->height != _height)
    {
        Log(L"Writer: channel %s has unexpected header", _name.c_str());
        Close();
        return false;
    }
    _header = header;
    Log(L"Writer: opened channel %s", _name.c_str());
    return true;
}

void ChannelWriter::Close()
{
    _header = nullptr;
    _view.reset();
    _event.reset();
    _mapping.reset();
}

HRESULT ChannelWriter::Write(const BYTE* nv12, UINT32 size)
{
    RETURN_HR_IF(E_INVALIDARG, size != Nv12FrameSize(_width, _height));
    if (!TryOpen())
        return S_FALSE;

    const LONG slot = (_header->latest + 1) % static_cast<LONG>(kChannelSlots);
    memcpy(SlotPtr(_view.get(), _header, slot), nv12, size);
    MemoryBarrier();
    InterlockedExchange(&_header->latest, slot);
    InterlockedIncrement64(&_header->seq);
    SetEvent(_event.get());
    return InUse() ? S_OK : S_FALSE;
}

bool ChannelWriter::InUse()
{
    if (!TryOpen())
        return false;
    const auto beat = static_cast<ULONGLONG>(_header->readerHeartbeat);
    return beat != 0 && GetTickCount64() - beat < 1000;
}
