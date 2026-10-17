#pragma once

// IMFAttributes の全メソッドを、メンバーの IMFAttributes へ転送する。
// Frame Server は Activator / MediaSource / MediaStream を IMFAttributes として扱うため、
// それぞれが自分自身で IMFAttributes を実装している必要がある。
#define FORWARD_IMFATTRIBUTES(a)                                                                                          \
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return a->GetItem(k, v); }                                 \
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return a->GetItemType(k, t); }                   \
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return a->CompareItem(k, v, r); }           \
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return a->Compare(o, m, r); }  \
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return a->GetUINT32(k, v); }                                  \
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return a->GetUINT64(k, v); }                                  \
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return a->GetDouble(k, v); }                                  \
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return a->GetGUID(k, v); }                                        \
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return a->GetStringLength(k, l); }                      \
    STDMETHODIMP GetString(REFGUID k, LPWSTR v, UINT32 n, UINT32* l) override { return a->GetString(k, v, n, l); }        \
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* v, UINT32* l) override { return a->GetAllocatedString(k, v, l); }  \
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* s) override { return a->GetBlobSize(k, s); }                              \
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* s) override { return a->GetBlob(k, b, n, s); }            \
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) override { return a->GetAllocatedBlob(k, b, s); }      \
    STDMETHODIMP GetUnknown(REFGUID k, REFIID i, LPVOID* p) override { return a->GetUnknown(k, i, p); }                   \
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return a->SetItem(k, v); }                               \
    STDMETHODIMP DeleteItem(REFGUID k) override { return a->DeleteItem(k); }                                              \
    STDMETHODIMP DeleteAllItems() override { return a->DeleteAllItems(); }                                                \
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return a->SetUINT32(k, v); }                                   \
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return a->SetUINT64(k, v); }                                   \
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return a->SetDouble(k, v); }                                   \
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return a->SetGUID(k, v); }                                      \
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return a->SetString(k, v); }                                  \
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return a->SetBlob(k, b, n); }                    \
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return a->SetUnknown(k, u); }                              \
    STDMETHODIMP LockStore() override { return a->LockStore(); }                                                          \
    STDMETHODIMP UnlockStore() override { return a->UnlockStore(); }                                                      \
    STDMETHODIMP GetCount(UINT32* c) override { return a->GetCount(c); }                                                  \
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return a->GetItemByIndex(i, k, v); }        \
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return a->CopyAllItems(d); }
