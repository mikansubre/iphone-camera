#include "pch.h"
#include "MediaStream.h"
#include "Log.h"

namespace
{
    constexpr UINT32 kFrameRates[] = {60, 30};
    // 新しいフレームが来ない間も、この間隔で同じ画を送り直してアプリのタイムアウトを防ぐ
    constexpr ULONGLONG kRepeatMs = 100;

    HRESULT CreateNv12Type(UINT32 width, UINT32 height, UINT32 fps, IMFMediaType** result)
    {
        wil::com_ptr_nothrow<IMFMediaType> type;
        RETURN_IF_FAILED(MFCreateMediaType(&type));
        RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
        RETURN_IF_FAILED(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, width, height));
        RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, fps, 1));
        RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
        RETURN_IF_FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        RETURN_IF_FAILED(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
        RETURN_IF_FAILED(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
        RETURN_IF_FAILED(type->SetUINT32(MF_MT_SAMPLE_SIZE, Nv12FrameSize(width, height)));
        RETURN_IF_FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, width));
        RETURN_IF_FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, width * height * 12 * fps));
        RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
        *result = type.detach();
        return S_OK;
    }
}

HRESULT MediaStream::RuntimeClassInitialize(IMFMediaSource* source, const std::wstring& channelName, UINT32 width,
                                            UINT32 height)
{
    _source = source;
    _channelName = channelName;
    _width = width;
    _height = height;

    RETURN_IF_FAILED(MFCreateAttributes(&_attributes, 4));
    RETURN_IF_FAILED(_attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    RETURN_IF_FAILED(_attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0));
    RETURN_IF_FAILED(_attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    RETURN_IF_FAILED(_attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
    RETURN_IF_FAILED(MFCreateEventQueue(&_queue));

    wil::com_ptr_nothrow<IMFMediaType> types[ARRAYSIZE(kFrameRates)];
    IMFMediaType* raw[ARRAYSIZE(kFrameRates)];
    for (size_t i = 0; i < ARRAYSIZE(kFrameRates); i++)
    {
        RETURN_IF_FAILED(CreateNv12Type(width, height, kFrameRates[i], &types[i]));
        raw[i] = types[i].get();
    }
    RETURN_IF_FAILED(MFCreateStreamDescriptor(0, ARRAYSIZE(raw), raw, &_descriptor));

    wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
    RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
    RETURN_IF_FAILED(handler->SetCurrentMediaType(raw[0]));

    RETURN_IF_FAILED(_wake.create(wil::EventOptions::None));
    RETURN_IF_FAILED(_stop.create(wil::EventOptions::ManualReset));
    return S_OK;
}

MediaStream::~MediaStream()
{
    StopWorker();
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator)
{
    RETURN_HR_IF_NULL(E_POINTER, allocator);
    std::lock_guard lock(_lock);
    _allocator.reset();
    return allocator->QueryInterface(IID_PPV_ARGS(&_allocator));
}

HRESULT MediaStream::Start(IMFMediaType* type)
{
    StopWorker();  // チャネルのハンドルを作り直すので、待機中のワーカーを先に止める
    {
        std::lock_guard lock(_lock);
        RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
        RETURN_HR_IF_MSG(MF_E_NOT_INITIALIZED, !_allocator, "no allocator");

        if (type)
        {
            UINT32 num = 0, den = 0;
            if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) && num)
                _frameDuration = static_cast<LONGLONG>(10'000'000ull * den / num);
            RETURN_IF_FAILED(_allocator->InitializeSampleAllocator(10, type));
        }

        if (FAILED(_channel.Create(_channelName, _width, _height)))
            Log(L"Stream: channel unavailable, sending black frames");

        _requests.clear();
        _deliveredSeq = -1;
        _delivered = 0;
        _state = MF_STREAM_STATE_RUNNING;
        RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
    }

    _stop.ResetEvent();
    _worker = std::thread([this] { Worker(); });
    Log(L"Stream started (frame duration %lld)", _frameDuration);
    return S_OK;
}

HRESULT MediaStream::Stop()
{
    StopWorker();
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

    if (_allocator)
        _allocator->UninitializeSampleAllocator();
    _channel.Close();
    _requests.clear();
    _state = MF_STREAM_STATE_STOPPED;
    Log(L"Stream stopped after %llu frames", _delivered);
    return _queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void MediaStream::Shutdown()
{
    StopWorker();
    std::lock_guard lock(_lock);
    if (_queue)
    {
        _queue->Shutdown();
        _queue.reset();
    }
    _channel.Close();
    _requests.clear();
    _allocator.reset();
    _descriptor.reset();
    _source.reset();  // MediaSource との循環参照を切る
}

void MediaStream::StopWorker()
{
    if (_worker.joinable())
    {
        _stop.SetEvent();
        _worker.join();
    }
}

void MediaStream::Worker()
{
    // 書き込み側のフレームが届いた瞬間、または新しい要求が来た瞬間に起きて、
    // 「未応答の要求」と「まだ渡していないフレーム」が揃ったら渡す
    while (true)
    {
        HANDLE handles[3] = {_stop.get(), _wake.get(), _channel.Event()};
        const DWORD count = handles[2] ? 3 : 2;
        if (WaitForMultipleObjects(count, handles, FALSE, 33) == WAIT_OBJECT_0)
            break;

        std::lock_guard lock(_lock);
        if (_state != MF_STREAM_STATE_RUNNING || _requests.empty() || !_queue)
            continue;

        const bool fresh = _channel.Seq() != _deliveredSeq;
        if (!fresh && GetTickCount64() - _lastDelivery < kRepeatMs)
            continue;

        auto token = std::move(_requests.front());
        _requests.pop_front();
        LOG_IF_FAILED(DeliverSample(token.get()));
    }
}

void MediaStream::FillFrame(BYTE* dst, LONG pitch)
{
    LONG64 seq = 0;
    if (_channel.CopyLatest(dst, pitch, &seq))
    {
        _deliveredSeq = seq;
        return;
    }

    // まだ書き込み側が来ていなければ黒 (limited range の Y=16, UV=128)
    for (UINT32 y = 0; y < _height; y++)
        memset(dst + static_cast<size_t>(y) * pitch, 16, _width);
    for (UINT32 y = _height; y < _height + _height / 2; y++)
        memset(dst + static_cast<size_t>(y) * pitch, 128, _width);
    _deliveredSeq = 0;
}

HRESULT MediaStream::DeliverSample(IUnknown* token)
{
    RETURN_HR_IF(MF_E_NOT_INITIALIZED, !_allocator);
    wil::com_ptr_nothrow<IMFSample> sample;
    RETURN_IF_FAILED(_allocator->AllocateSample(&sample));
    wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));

    if (auto buffer2d = buffer.try_query<IMF2DBuffer2>())
    {
        BYTE* scanline0 = nullptr;
        BYTE* start = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        RETURN_IF_FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &start, &length));
        FillFrame(scanline0, pitch);
        buffer2d->Unlock2D();
    }
    else
    {
        BYTE* data = nullptr;
        DWORD maxLength = 0;
        RETURN_IF_FAILED(buffer->Lock(&data, &maxLength, nullptr));
        FillFrame(data, static_cast<LONG>(_width));
        buffer->Unlock();
        RETURN_IF_FAILED(buffer->SetCurrentLength(Nv12FrameSize(_width, _height)));
    }

    RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
    RETURN_IF_FAILED(sample->SetSampleDuration(_frameDuration));
    if (token)
        RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, token));
    RETURN_IF_FAILED(_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.get()));

    _channel.Heartbeat();
    _lastDelivery = GetTickCount64();
    if (++_delivered == 1)
        Log(L"Stream: first sample delivered (2D buffer: %s)", buffer.try_query<IMF2DBuffer2>() ? L"yes" : L"no");
    return S_OK;
}

// IMFMediaEventGenerator

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
    return _queue->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
    return _queue->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    wil::com_ptr_nothrow<IMFMediaEventQueue> queue;
    {
        std::lock_guard lock(_lock);
        RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
        queue = _queue;
    }
    return queue->GetEvent(flags, event);  // ブロックし得るのでロックの外で待つ
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                     const PROPVARIANT* eventValue)
{
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
    return _queue->QueueEventParamVar(type, extendedType, status, eventValue);
}

// IMFMediaStream

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source)
{
    RETURN_HR_IF_NULL(E_POINTER, source);
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_source);
    return _source.copy_to(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor)
{
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);
    return _descriptor.copy_to(descriptor);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token)
{
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
    RETURN_HR_IF(MF_E_INVALIDREQUEST, _state != MF_STREAM_STATE_RUNNING);
    _requests.emplace_back(token);
    _wake.SetEvent();
    return S_OK;
}

// IMFMediaStream2

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE newState)
{
    Log(L"Stream: SetStreamState %d -> %d", _state, newState);
    if (newState == _state)
        return S_OK;
    switch (newState)
    {
    case MF_STREAM_STATE_PAUSED:
    {
        std::lock_guard lock(_lock);
        RETURN_HR_IF(MF_E_INVALID_STATE_TRANSITION, _state != MF_STREAM_STATE_RUNNING);
        _state = newState;
        return S_OK;
    }
    case MF_STREAM_STATE_RUNNING:
        return Start(nullptr);
    case MF_STREAM_STATE_STOPPED:
        return Stop();
    default:
        return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* currentState)
{
    RETURN_HR_IF_NULL(E_POINTER, currentState);
    *currentState = _state;
    return S_OK;
}

// IKsControl: カメラ制御 (露出・ズーム等) は公開しない

STDMETHODIMP_(NTSTATUS) MediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
