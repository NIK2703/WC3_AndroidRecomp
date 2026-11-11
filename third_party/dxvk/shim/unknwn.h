/*
 * unknwn.h - COM base interface, for the Android DXVK build.
 *
 * DXVK includes <unknwn.h> for IUnknown and then compiles the Wine DirectX
 * headers, which use the standard MinGW/MSVC interface macros.  The macros
 * live in windows.h here; this header only adds the interfaces themselves.
 */
#ifndef WC3_ANDROID_DXVK_UNKNWN_H
#define WC3_ANDROID_DXVK_UNKNWN_H

#include "windows.h"

/* {00000000-0000-0000-C000-000000000046} */
#ifndef IID_IUnknown_DEFINED
#define IID_IUnknown_DEFINED
DEFINE_GUID(IID_IUnknown,
    0x00000000, 0x0000, 0x0000,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
#endif

/* {00000001-0000-0000-C000-000000000046} */
#ifndef IID_IClassFactory_DEFINED
#define IID_IClassFactory_DEFINED
DEFINE_GUID(IID_IClassFactory,
    0x00000001, 0x0000, 0x0000,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
#endif

DECLARE_INTERFACE(IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
};

DECLARE_INTERFACE_(IClassFactory, IUnknown)
{
    STDMETHOD(CreateInstance)(THIS_ IUnknown *outer, REFIID riid, void **ppv) PURE;
    STDMETHOD(LockServer)(THIS_ BOOL lock) PURE;
};

/* `__uuidof(IUnknown)` and `__uuidof(IClassFactory)` are both used, and the
 * function-template emulation in windows.h needs an explicit specialisation
 * per type - DEFINE_GUID alone does not provide one. */
#ifdef __cplusplus
__CRT_UUID_DECL(IUnknown,
    0x00000000, 0x0000, 0x0000,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
__CRT_UUID_DECL(IClassFactory,
    0x00000001, 0x0000, 0x0000,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
#endif

#endif
