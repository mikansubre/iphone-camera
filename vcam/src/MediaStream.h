#pragma once

#include "Attributes.h"
#include "FrameChannel.h"

class MediaStream
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>, IMFAttributes,
          IKsControl>
{
public:
    HRESULT RuntimeClassInitialize(IMFMediaSource* source, const std::wstring& channelName, UINT32 width,
                                   UINT32 height);
    ~MediaStream();

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                            const PROPVARIANT* eventValue) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE newState) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* currentState) override;

    // IKsControl
    STDMETHODIMP_(NTSTATUS) KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP_(NTSTATUS) KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP_(NTSTATUS) KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

    FORWARD_IMFATTRIBUTES(_attributes)

    HRESULT SetAllocator(IUnknown* allocator);
    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();

private:
    void Worker();
    HRESULT DeliverSample(IUnknown* token);
    void FillFrame(BYTE* dst, LONG pitch);
    void StopWorker();

    std::mutex _lock;
    MF_STREAM_STATE _state = MF_STREAM_STATE_STOPPED;
    UINT32 _width = 0, _height = 0;
    LONGLONG _frameDuration = 166667;
    std::wstring _channelName;
    ChannelReader _channel;

    wil::com_ptr_nothrow<IMFAttributes> _attributes;
    wil::com_ptr_nothrow<IMFMediaEventQueue> _queue;
    wil::com_ptr_nothrow<IMFStreamDescriptor> _descriptor;
    wil::com_ptr_nothrow<IMFMediaSource> _source;
    wil::com_ptr_nothrow<IMFVideoSampleAllocatorEx> _allocator;

    // RequestSample で受け取った、まだ応えていない要求 (token は null のこともある)
    std::deque<wil::com_ptr_nothrow<IUnknown>> _requests;
    LONG64 _deliveredSeq = -1;
    ULONGLONG _lastDelivery = 0;
    UINT64 _delivered = 0;

    wil::unique_event_nothrow _wake;
    wil::unique_event_nothrow _stop;
    std::thread _worker;
};
