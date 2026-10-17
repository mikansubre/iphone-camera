#pragma once

#include "Attributes.h"

#include "MediaStream.h"

class MediaSource
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>, IMFAttributes,
          IMFGetService, IKsControl, IMFSampleAllocatorControl>
{
public:
    HRESULT RuntimeClassInitialize(IMFAttributes* activatorAttributes, const std::wstring& friendlyName);

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                            const PROPVARIANT* eventValue) override;

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                       const PROPVARIANT* startPosition) override;
    STDMETHODIMP Stop() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD streamId, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

    // IKsControl
    STDMETHODIMP_(NTSTATUS) KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP_(NTSTATUS) KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP_(NTSTATUS) KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

    FORWARD_IMFATTRIBUTES(_attributes)

private:
    std::mutex _lock;
    wil::com_ptr_nothrow<IMFAttributes> _attributes;
    wil::com_ptr_nothrow<IMFMediaEventQueue> _queue;
    wil::com_ptr_nothrow<IMFPresentationDescriptor> _descriptor;
    Microsoft::WRL::ComPtr<MediaStream> _stream;  // ストリームは 1 本だけ
};
