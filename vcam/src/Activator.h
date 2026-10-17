#pragma once

#include "Attributes.h"

// Frame Server がカメラを開くときに最初に作るオブジェクト。ActivateObject で MediaSource を返す。
class Activator
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::ChainInterfaces<IMFActivate, IMFAttributes>>
{
public:
    HRESULT RuntimeClassInitialize();

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** object) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    FORWARD_IMFATTRIBUTES(_attributes)

private:
    std::wstring ResolveFriendlyName();

    std::mutex _lock;
    wil::com_ptr_nothrow<IMFAttributes> _attributes;
    wil::com_ptr_nothrow<IMFMediaSource> _source;
};
