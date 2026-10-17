#pragma once

// iPhoneCam の映像受け渡し用の共有メモリ (NV12 のフレームを 3 枚分のリングで持つ)。
//
// Frame Server (session 0 の LocalService) 側の MediaStream が Reader として Global\ 名前空間に作り、
// server.py のプロセス側 (Writer) はそれを開いて書き込む。Global\ への作成は特権が要るので、
// 作るのは必ずサービス側。アプリがカメラを使っていない間は存在しないので、Writer は書き込みを省ける。
// 名前はカメラのフレンドリ名から決める (MediaSource が自分のカメラを特定できる唯一の手がかり)。

constexpr UINT32 kChannelMagic = 0x4D414349;  // 'ICAM'
constexpr UINT32 kChannelVersion = 1;
constexpr UINT32 kChannelSlots = 3;
constexpr size_t kChannelHeaderSize = 4096;

struct ChannelHeader
{
    UINT32 magic;
    UINT32 version;
    UINT32 width;
    UINT32 height;
    UINT32 frameSize;
    UINT32 slots;
    volatile LONG64 seq;              // 書き込み済みフレーム数。スロットを書き終えてから増やす
    volatile LONG latest;             // 最新フレームのスロット番号
    volatile LONG64 readerHeartbeat;  // Reader が最後にフレームを渡した時刻 (GetTickCount64)
};

std::wstring ChannelName(const std::wstring& friendlyName);
UINT32 Nv12FrameSize(UINT32 width, UINT32 height);

class ChannelReader
{
public:
    HRESULT Create(const std::wstring& name, UINT32 width, UINT32 height);
    void Close();
    HANDLE Event() const { return _event.get(); }
    LONG64 Seq() const { return _header ? _header->seq : 0; }
    // 最新フレームを dst へコピーする (pitch は Y/UV 共通の行間隔)。まだ一度も書かれていなければ false
    bool CopyLatest(BYTE* dst, LONG pitch, LONG64* seq);
    void Heartbeat();

private:
    wil::unique_handle _mapping;
    wil::unique_event_nothrow _event;
    wil::unique_mapview_ptr<BYTE> _view;
    ChannelHeader* _header = nullptr;
};

class ChannelWriter
{
public:
    void Init(const std::wstring& name, UINT32 width, UINT32 height);
    // S_OK: 書き込んだ, S_FALSE: 使っているアプリが無い
    HRESULT Write(const BYTE* nv12, UINT32 size);
    bool InUse();

private:
    bool TryOpen();
    void Close();

    std::wstring _name;
    UINT32 _width = 0, _height = 0;
    ULONGLONG _nextOpenTry = 0;
    wil::unique_handle _mapping;
    wil::unique_event_nothrow _event;
    wil::unique_mapview_ptr<BYTE> _view;
    ChannelHeader* _header = nullptr;
};
