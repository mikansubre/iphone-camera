#include "pch.h"
#include "MediaSource.h"
#include "MediaStream.h"
#include "Config.h"
#include "FrameChannel.h"
#include "Log.h"

HRESULT MediaSource::RuntimeClassInitialize(IMFAttributes* activatorAttributes, const std::wstring& friendlyName)
{
    RETURN_IF_FAILED(MFCreateAttributes(&_attributes, 8));
    if (activatorAttributes)
        RETURN_IF_FAILED(activatorAttributes->CopyAllItems(_attributes.get()));

    // 30fps 以下 (通常) と 60fps (高フレームレート) のプロファイルを公開する
    wil::com_ptr_nothrow<IMFSensorProfileCollection> profiles;
    RETURN_IF_FAILED(MFCreateSensorProfileCollection(&profiles));
    wil::com_ptr_nothrow<IMFSensorProfile> profile;
    RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile));
    RETURN_IF_FAILED(profile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))"));
    RETURN_IF_FAILED(profiles->AddProfile(profile.get()));
    RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_HighFrameRate, 0, nullptr, &profile));
    RETURN_IF_FAILED(profile->AddProfileFilter(0, L"((RES==;FRT>=60,1;SUT==))"));
    RETURN_IF_FAILED(profiles->AddProfile(profile.get()));
    RETURN_IF_FAILED(_attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.get()));

    const auto config = LoadConfig();
    const auto channel = ChannelName(friendlyName);
    Log(L"MediaSource for '%s' -> %s (%ux%u)", friendlyName.c_str(), channel.c_str(), config.width, config.height);
    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaStream>(&_stream, this, channel, config.width,
                                                                    config.height));

    wil::com_ptr_nothrow<IMFStreamDescriptor> streamDescriptor;
    RETURN_IF_FAILED(_stream->GetStreamDescriptor(&streamDescriptor));
    IMFStreamDescriptor* descriptors[] = {streamDescriptor.get()};
    RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, descriptors, &_descriptor));
    RETURN_IF_FAILED(MFCreateEventQueue(&_queue));
    return S_OK;
}

// IMFMediaEventGenerator

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
    return _queue->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
    return _queue->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    wil::com_ptr_nothrow<IMFMediaEventQueue> queue;
    {
        std::lock_guard lock(_lock);
        RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
        queue = _queue;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                     const PROPVARIANT* eventValue)
{
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
    return _queue->QueueEventParamVar(type, extendedType, status, eventValue);
}

// IMFMediaSource

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor)
{
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);
    return _descriptor->Clone(descriptor);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics)
{
    RETURN_HR_IF_NULL(E_POINTER, characteristics);
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause()
{
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP MediaSource::Shutdown()
{
    Log(L"MediaSource::Shutdown");
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
    _queue->Shutdown();
    _queue.reset();
    if (_stream)
        _stream->Shutdown();
    _stream.Reset();
    _descriptor.reset();
    return S_OK;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                                const PROPVARIANT* startPosition)
{
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    RETURN_HR_IF_NULL(E_POINTER, startPosition);
    RETURN_HR_IF(MF_E_UNSUPPORTED_TIME_FORMAT, timeFormat && *timeFormat != GUID_NULL);
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_descriptor);

    DWORD count = 0;
    RETURN_IF_FAILED(descriptor->GetStreamDescriptorCount(&count));
    RETURN_HR_IF(E_INVALIDARG, count != 1);

    BOOL selected = FALSE;
    wil::com_ptr_nothrow<IMFStreamDescriptor> streamDescriptor;
    RETURN_IF_FAILED(descriptor->GetStreamDescriptorByIndex(0, &selected, &streamDescriptor));

    MF_STREAM_STATE state;
    RETURN_IF_FAILED(_stream->GetStreamState(&state));
    const bool running = state != MF_STREAM_STATE_STOPPED;
    Log(L"MediaSource::Start selected:%d running:%d", selected, running);

    if (selected && !running)
    {
        RETURN_IF_FAILED(_descriptor->SelectStream(0));
        RETURN_IF_FAILED(_queue->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK,
                                                   static_cast<IMFMediaStream*>(_stream.Get())));
        wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
        wil::com_ptr_nothrow<IMFMediaType> type;
        RETURN_IF_FAILED(streamDescriptor->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));
        RETURN_IF_FAILED(_stream->Start(type.get()));
    }
    else if (!selected && running)
    {
        RETURN_IF_FAILED(_descriptor->DeselectStream(0));
        RETURN_IF_FAILED(_stream->Stop());
    }

    wil::unique_prop_variant time;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));
    return _queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
}

STDMETHODIMP MediaSource::Stop()
{
    std::lock_guard lock(_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_descriptor);
    RETURN_IF_FAILED(_stream->Stop());
    RETURN_IF_FAILED(_descriptor->DeselectStream(0));

    wil::unique_prop_variant time;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));
    return _queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &time);
}

// IMFMediaSourceEx

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes)
{
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    return QueryInterface(IID_PPV_ARGS(attributes));
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes)
{
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    *attributes = nullptr;
    std::lock_guard lock(_lock);
    RETURN_HR_IF(E_INVALIDARG, streamId != 0);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_stream);
    return _stream.CopyTo(attributes);
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*)
{
    // GPU のテクスチャではなくシステムメモリのサンプルを使う (共有メモリから CPU でコピーするため)
    return S_OK;
}

// IMFGetService

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object)
{
    if (object)
        *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// IMFSampleAllocatorControl

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD streamId, IUnknown* allocator)
{
    std::lock_guard lock(_lock);
    RETURN_HR_IF(E_INVALIDARG, streamId != 0);
    RETURN_HR_IF(MF_E_SHUTDOWN, !_stream);
    return _stream->SetAllocator(allocator);
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage)
{
    RETURN_HR_IF_NULL(E_POINTER, inputStreamId);
    RETURN_HR_IF_NULL(E_POINTER, usage);
    RETURN_HR_IF(E_INVALIDARG, streamId != 0);
    *inputStreamId = streamId;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

// IKsControl

STDMETHODIMP_(NTSTATUS) MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
