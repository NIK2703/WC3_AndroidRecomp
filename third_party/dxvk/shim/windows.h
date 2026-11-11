/*
 * windows.h - Win32 surface for building DXVK for Android/arm64.
 *
 * This is NOT the same header as runtime/android/win32_compat.h.  That one
 * exists to satisfy the project's own renderer, which touches eleven Win32
 * functions.  DXVK is a Win32 library: it wants COM, real synchronisation
 * primitives, threads, wide strings and monitor enumeration, and it wants them
 * to actually behave like Win32 rather than merely compile.
 *
 * The definitions below are therefore split in two:
 *
 *   - types and macros, which are pure declarations and are safe to supply;
 *   - functions, which are either implemented inline here on top of POSIX, or
 *     declared and left to runtime/android/dxvk_win32.c so that the Win32
 *     semantics (handle tables, event resets, wait ordering) live in one place
 *     instead of being sprinkled through a header.
 *
 * Anything still missing will show up as a compile error, which is the point:
 * this file is grown by measurement, not by guessing.
 */
#ifndef WC3_ANDROID_DXVK_WINDOWS_H
#define WC3_ANDROID_DXVK_WINDOWS_H

#if !defined(__ANDROID__)
#error "dxvk_shim/windows.h is for the Android build only"
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <time.h>
#include <type_traits>
#include <pthread.h>

/* DXVK branches on _WIN32 in a dozen places (intrinsics, path handling, the
 * WSI backend).  It is a Win32 library; tell it so. */
#ifndef _WIN32
#define _WIN32 1
#endif
/* ARM64 Android is LP64, i.e. the Win64 data model.  DXVK keys a few
 * decisions on this macro, most notably DXVK_USE_UNMAPPABLE_MEMORY, which
 * exists to conserve 32-bit address space and must stay off here. */
#if (defined(__LP64__) || defined(_LP64)) && !defined(_WIN64)
#define _WIN64 1
#endif
#ifndef WIN32
#define WIN32 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

/* The DirectX headers in third_party/dxvk-src are newer than the ones DXVK
 * 1.10.3 was written against, so a few types it defines defensively already
 * exist there.  DXVK's own guards key on this macro; setting it here keeps
 * the decision in one place instead of patching each definition. */
#define DXVK_HAVE_NATIVE_D3D11_FORMAT_SUPPORT2

/* ---------------------------------------------------------------- types */

typedef uint8_t   BYTE;
typedef uint16_t  WORD;
typedef uint32_t  DWORD;
/* `char`, not int8_t: on Windows CHAR is the plain char type, so LPSTR has to
 * be `char *` or every DXVK call that passes a char* fails to bind. */
typedef char      CHAR;
typedef int16_t   SHORT;
typedef uint16_t  USHORT;
typedef int32_t   LONG;
typedef uint32_t  ULONG;
typedef int32_t   INT;
typedef uint32_t  UINT;
typedef int64_t   LONGLONG;
typedef uint64_t  ULONGLONG;
typedef uint64_t  DWORDLONG;
typedef float     FLOAT;
typedef double    DOUBLE;
typedef int       BOOL;
typedef uint8_t   UCHAR;
typedef uint32_t  ULONG32;
typedef uint64_t  ULONG64;
typedef uintptr_t ULONG_PTR;
typedef intptr_t  LONG_PTR;
typedef unsigned char BOOLEAN;
typedef int32_t   NTSTATUS;

#define HandleToULong(h)  ((ULONG)(uintptr_t)(h))
#define HandleToUlong(h)  ((ULONG)(uintptr_t)(h))
#define ULongToHandle(u)  ((HANDLE)(uintptr_t)(u))
#define LongToHandle(l)   ((HANDLE)(intptr_t)(l))
#define HandleToLong(h)   ((LONG)(intptr_t)(h))

#define STATUS_SUCCESS      ((NTSTATUS)0x00000000L)
#define STATUS_TIMEOUT      ((NTSTATUS)0x00000102L)
#define STATUS_PENDING      ((NTSTATUS)0x00000103L)
typedef uintptr_t DWORD_PTR;
typedef uintptr_t UINT_PTR;
typedef uintptr_t SIZE_T;
typedef intptr_t  SSIZE_T;
typedef uint64_t  UINT64;
typedef int64_t   INT64;
typedef uint32_t  UINT32;
typedef int32_t   INT32;
typedef uint16_t  UINT16;
typedef int16_t   INT16;
typedef uint8_t   UINT8;
typedef int8_t    INT8;
typedef char      CHAR8;

/* Windows wchar_t is 16 bits.  The Android toolchain's is 32, so the build
 * passes -fshort-wchar and these aliases are then correct. */
typedef wchar_t   WCHAR;
typedef WCHAR     TCHAR;
typedef CHAR     *LPSTR;
typedef const CHAR *LPCSTR;
typedef WCHAR    *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef const WCHAR *PCWSTR;
typedef WCHAR    *PWSTR;
typedef void     *LPVOID;
typedef const void *LPCVOID;
typedef void     *PVOID;
typedef void      VOID;
typedef int32_t   HRESULT;
typedef LONG      NTSTATUS;
typedef LONG      LSTATUS;

typedef void     *HANDLE;
typedef void     *HWND;
/* Message plumbing.  DXVK's swapchain subclasses the window, so the window
 * procedure type and the message argument types have to exist even though
 * nothing here ever dispatches a message. */
typedef intptr_t  LRESULT;
typedef uintptr_t WPARAM;
typedef intptr_t  LPARAM;
typedef void     *HINSTANCE;
typedef void     *HMODULE;
typedef void     *HDC;
typedef void     *HMONITOR;
typedef LRESULT (__stdcall *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void     *HICON;
typedef void     *HRGN;
typedef void     *HPEN;
typedef void     *HBRUSH;
typedef void     *HCURSOR;
typedef void     *HKEY;
typedef void     *HGDIOBJ;
typedef struct HINSTANCE__ *HINSTANCE_PTR;

typedef int       WINBOOL;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif

#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000u
#define CREATE_SUSPENDED 0x00000004u
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu

/* Access rights.  util_file.cpp opens the shader cache through these. */
#define GENERIC_READ    0x80000000u
#define GENERIC_WRITE   0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL     0x10000000u

/* File mapping rights.  util_unmap.cpp maps a section. */
#define FILE_MAP_COPY       0x00000001u
#define FILE_MAP_WRITE      0x00000002u
#define FILE_MAP_READ       0x0004u
#define FILE_MAP_EXECUTE    0x0020u
#define FILE_MAP_ALL_ACCESS 0x000F001Fu

#define WAIT_OBJECT_0 0x00000000u
#define WAIT_TIMEOUT  0x00000102u
#define WAIT_FAILED   0xFFFFFFFFu

#define ERROR_SUCCESS 0u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_OUTOFMEMORY 14u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u

typedef struct tagRECT {
    LONG left, top, right, bottom;
} RECT, *LPRECT;
typedef const RECT *LPCRECT;

typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT;
typedef struct tagSIZE { LONG cx, cy; } SIZE, *LPSIZE;

typedef union {
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union {
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

/* A LUID is an 8-byte adapter identifier.  DXVK only ever stores and compares
 * it, so the layout is all that matters. */
typedef struct _LUID {
    DWORD LowPart;
    LONG  HighPart;
} LUID, *PLUID;

/* Thread priorities: DXVK names them, nothing here acts on them. */
#define THREAD_PRIORITY_IDLE             -15
#define THREAD_PRIORITY_LOWEST           -2
#define THREAD_PRIORITY_BELOW_NORMAL     -1
#define THREAD_PRIORITY_NORMAL           0
#define THREAD_PRIORITY_ABOVE_NORMAL     1
#define THREAD_PRIORITY_HIGHEST          2
#define THREAD_PRIORITY_TIME_CRITICAL    15

/* Security descriptors are a no-op for us; the struct exists so a parameter
 * list compiles.  DXVK only ever passes NULL. */
typedef struct _SECURITY_ATTRIBUTES {
    DWORD  nLength;
    void  *lpSecurityDescriptor;
    BOOL   bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

/* SYSTEM_INFO: DXVK reads dwNumberOfProcessors and dwPageSize to size its
 * worker pools, so the real values are filled in by GetSystemInfo rather than
 * left zero. */
typedef struct _SYSTEM_INFO {
    union {
        DWORD dwOemId;
        struct {
            WORD wProcessorArchitecture;
            WORD wReserved;
        } s;
    } u;
    DWORD     dwPageSize;
    void     *lpMinimumApplicationAddress;
    void     *lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors;
    DWORD     dwProcessorType;
    DWORD     dwAllocationGranularity;
    WORD      wProcessorLevel;
    WORD      wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

#define PROCESSOR_ARCHITECTURE_ARM64 12
#define PROCESSOR_ARCHITECTURE_AMD64 9
#define PROCESSOR_ARCHITECTURE_INTEL 0

/* Display modes.  DXVK enumerates them through EnumDisplaySettingsW to build
 * its mode list, so the field offsets have to match the Windows struct. */
typedef struct _devicemodeA {
    BYTE  dmDeviceName[32];
    WORD  dmSpecVersion;
    WORD  dmDriverVersion;
    WORD  dmSize;
    WORD  dmDriverExtra;
    DWORD dmFields;
    LONG  dmPositionX;
    LONG  dmPositionY;
    DWORD dmDisplayOrientation;
    DWORD dmDisplayFixedOutput;
    SHORT dmColor;
    SHORT dmDuplex;
    SHORT dmYResolution;
    SHORT dmTTOption;
    SHORT dmCollate;
    BYTE  dmFormName[32];
    WORD  dmLogPixels;
    DWORD dmBitsPerPel;
    DWORD dmPelsWidth;
    DWORD dmPelsHeight;
    DWORD dmDisplayFlags;
    DWORD dmDisplayFrequency;
    DWORD dmICMMethod;
    DWORD dmICMIntent;
    DWORD dmMediaType;
    DWORD dmDitherType;
    DWORD dmReserved1;
    DWORD dmReserved2;
    DWORD dmPanningWidth;
    DWORD dmPanningHeight;
} DEVMODEA, *PDEVMODEA, *LPDEVMODEA;

typedef struct _devicemodeW {
    WCHAR dmDeviceName[32];
    WORD  dmSpecVersion;
    WORD  dmDriverVersion;
    WORD  dmSize;
    WORD  dmDriverExtra;
    DWORD dmFields;
    LONG  dmPositionX;
    LONG  dmPositionY;
    DWORD dmDisplayOrientation;
    DWORD dmDisplayFixedOutput;
    SHORT dmColor;
    SHORT dmDuplex;
    SHORT dmYResolution;
    SHORT dmTTOption;
    SHORT dmCollate;
    WCHAR dmFormName[32];
    WORD  dmLogPixels;
    DWORD dmBitsPerPel;
    DWORD dmPelsWidth;
    DWORD dmPelsHeight;
    DWORD dmDisplayFlags;
    DWORD dmDisplayFrequency;
    DWORD dmICMMethod;
    DWORD dmICMIntent;
    DWORD dmMediaType;
    DWORD dmDitherType;
    DWORD dmReserved1;
    DWORD dmReserved2;
    DWORD dmPanningWidth;
    DWORD dmPanningHeight;
} DEVMODEW, *PDEVMODEW, *LPDEVMODEW;

#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#define DM_PELSWIDTH  0x00080000u
#define DM_PELSHEIGHT 0x00100000u
#define DM_BITSPERPEL 0x00040000u
#define DM_DISPLAYFREQUENCY 0x00400000u

typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} GUID, IID, CLSID;

/* REFGUID/REFIID/REFCLSID are *references* in C++, exactly as in the Windows
 * SDK and in Wine's guiddef.h.  DXVK depends on that in both directions: it
 * reads `riid.Data1` and it compares `riid == __uuidof(X)` by value.  The C
 * form has to stay a pointer for source compatibility. */
#ifdef __cplusplus
#define REFGUID  const GUID &
#define REFIID   const IID &
#define REFCLSID const CLSID &
#else
typedef const GUID  *REFGUID;
typedef const IID   *REFIID;
typedef const CLSID *REFCLSID;
#endif
typedef GUID *LPGUID;
typedef GUID *LPIID;
typedef GUID *LPCLSID;

#ifndef CONST
#define CONST const
#endif
#ifndef __MSABI_LONG
#define __MSABI_LONG(x) ((unsigned long)(x))
#endif

#ifdef __cplusplus
#define WC3_GUID_INIT(a,b,c,d0,d1,d2,d3,d4,d5,d6,d7) \
    { (a), (b), (c), { (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) } }
#else
#define WC3_GUID_INIT(a,b,c,d0,d1,d2,d3,d4,d5,d6,d7) \
    { (a), (b), (c), { (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) } }
#endif

#ifndef DEFINE_GUID
#define DEFINE_GUID(name, a,b,c, d0,d1,d2,d3,d4,d5,d6,d7) \
    const GUID name = WC3_GUID_INIT(a,b,c,d0,d1,d2,d3,d4,d5,d6,d7)
#endif
#ifndef EXTERN_C
#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif
#endif

#ifdef __cplusplus
inline bool operator==(const GUID &a, const GUID &b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           memcmp(a.Data4, b.Data4, sizeof a.Data4) == 0;
}
inline bool operator!=(const GUID &a, const GUID &b) { return !(a == b); }

/* On Windows REFGUID/REFIID are *references*, so `riid == __uuidof(X)` is a
 * plain value comparison.  This shim keeps them as pointers (Wine's DirectX
 * headers dereference them), so the mixed forms have to exist explicitly or
 * every `riid == IID_...` in DXVK becomes "invalid operands to binary
 * expression".  The pointer overloads dereference, which is what the SDK
 * semantics require; a null reference has no Windows equivalent, so it
 * simply never compares equal. */
inline bool operator==(const GUID *a, const GUID &b) { return a && *a == b; }
inline bool operator!=(const GUID *a, const GUID &b) { return !(a == b); }
inline bool operator==(const GUID &a, const GUID *b) { return b && a == *b; }
inline bool operator!=(const GUID &a, const GUID *b) { return !(a == b); }
#endif

/* IsEqualGUID takes *references*, matching the Windows SDK.  DXVK calls it
 * with __uuidof(X) on one side, which yields a GUID lvalue, so a pointer
 * signature would not bind. */
static inline BOOL IsEqualGUID(const GUID &a, const GUID &b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}
#define IsEqualIID(a, b) IsEqualGUID(a, b)
#define IsEqualCLSID(a, b) IsEqualGUID(a, b)

/* ------------------------------------------------------------- HRESULT */

#define S_OK          ((HRESULT)0L)
#define S_FALSE       ((HRESULT)1L)
#define E_FAIL        ((HRESULT)0x80004005L)
#define E_INVALIDARG  ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_NOTIMPL     ((HRESULT)0x80004001L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_POINTER     ((HRESULT)0x80004003L)

/* DXGI status codes.  dxgi.h defines these too, but the private-data code in
 * util/com needs them without dragging the whole DXGI header in. */
#define DXGI_ERROR_INVALID_CALL  ((HRESULT)0x887A0001L)
#define DXGI_ERROR_NOT_FOUND     ((HRESULT)0x887A0002L)
#define DXGI_ERROR_MORE_DATA     ((HRESULT)0x887A0003L)
#define DXGI_ERROR_UNSUPPORTED   ((HRESULT)0x887A0004L)
#define DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ((HRESULT)0x887A0022L)
#define E_POINTER     ((HRESULT)0x80004003L)

#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr)    (((HRESULT)(hr)) < 0)
#define MAKE_HRESULT(sev, fac, code) \
    ((HRESULT)(((uint32_t)(sev) << 31) | ((uint32_t)(fac) << 16) | (uint32_t)(code)))
#define MAKE_HRESULT_SEVERITY(hr) (((HRESULT)(hr) >> 31) & 1)
#define HRESULT_CODE(hr)  ((hr) & 0xFFFF)
#define HRESULT_FACILITY(hr) (((hr) >> 16) & 0x1FFF)

/* ---------------------------------------------------- calling convention */

#ifndef __stdcall
#define __stdcall
#endif
#ifndef __cdecl
#define __cdecl
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE __stdcall
#endif
#ifndef NTAPI
#define NTAPI __stdcall
#endif
#ifndef STDAPICALLTYPE
#define STDAPICALLTYPE __stdcall
#endif

/* The Win32 SDK sprinkles these over its prototypes.  They exist to carry
 * __declspec(dllimport) in the import libraries; here they carry nothing. */
#ifndef WINUSERAPI
#define WINUSERAPI
#endif
#ifndef WINBASEAPI
#define WINBASEAPI
#endif
#ifndef WINGDIAPI
#define WINGDIAPI
#endif
#ifndef WINADVAPI
#define WINADVAPI
#endif
#ifndef WINCOMMCTRLAPI
#define WINCOMMCTRLAPI
#endif
#ifndef NTSYSAPI
#define NTSYSAPI
#endif

/* ----------------------------------------------------------- COM macros
 *
 * DXVK compiles its interfaces from the Wine DirectX headers, which expect
 * the MinGW/MSVC macro set.  These are the standard definitions. */
#ifndef PURE
#define PURE = 0
#endif
#ifndef STDMETHOD
#define STDMETHOD(method) virtual HRESULT STDMETHODCALLTYPE method
#endif
#ifndef STDMETHOD_
#define STDMETHOD_(type, method) virtual type STDMETHODCALLTYPE method
#endif
#ifndef STDMETHODIMP
#define STDMETHODIMP HRESULT STDMETHODCALLTYPE
#endif
#ifndef STDMETHODIMP_
#define STDMETHODIMP_(type) type STDMETHODCALLTYPE
#endif
#ifndef THIS_
#define THIS_
#endif
#ifndef THIS
#define THIS void
#endif
#ifndef DECLARE_INTERFACE
#define DECLARE_INTERFACE(iface) struct iface
#endif
#ifndef DECLARE_INTERFACE_
#define DECLARE_INTERFACE_(iface, baseiface) struct iface : public baseiface
#endif
#ifndef interface
#define interface struct
#endif

/* The Wine DirectX headers open every COM interface with MIDL_INTERFACE and
 * bracket the vtable-only (C) form with BEGIN_INTERFACE / END_INTERFACE.
 * MinGW defines all three as no-ops; without them `MIDL_INTERFACE("...")`
 * parses as a top-level declaration with a stray string literal, which is
 * the "a type specifier is required for all declarations" seen in every
 * d3d9/d3d11 TU. */
#ifndef MIDL_INTERFACE
#define MIDL_INTERFACE(x) struct
#endif
#ifndef BEGIN_INTERFACE
#define BEGIN_INTERFACE
#endif
#ifndef END_INTERFACE
#define END_INTERFACE
#endif
#ifndef DECLSPEC_UUID
#define DECLSPEC_UUID(x)
#endif
#ifndef DECLSPEC_NOVTABLE
#define DECLSPEC_NOVTABLE
#endif
#ifndef DECLSPEC_SELECTANY
#define DECLSPEC_SELECTANY
#endif

/* The DirectX headers put their unions in unnamed-anonymous form by leaving
 * these empty.  Naming them instead (which is what happens if they are simply
 * undefined) makes `D3DMATRIX.m` unreachable and turns every `.m` / `._11`
 * access in d3d9_util.h into a hard error.  Nothing in the headers uses them
 * as accessor names, so empty is correct and keeps the layout identical. */
#define DUMMYUNIONNAME
#define DUMMYUNIONNAME1
#define DUMMYUNIONNAME2
#define DUMMYUNIONNAME3
#define DUMMYUNIONNAME4
#define DUMMYUNIONNAME5
#define DUMMYUNIONNAME6
#define DUMMYUNIONNAME7
#define DUMMYUNIONNAME8
#define DUMMYSTRUCTNAME

/* The DirectX headers mark anonymous members with __C89_NAMELESS, which on
 * GCC/Clang is just `__extension__` - it silences the pedantic diagnostic for
 * the anonymous struct/union.  The NAME variants stay empty for the same
 * reason as DUMMY* above: naming them would break direct member access. */
#ifndef __C89_NAMELESS
#define __C89_NAMELESS __extension__
#endif
#define __C89_NAMELESSUNIONNAME
#define __C89_NAMELESSUNIONNAME1
#define __C89_NAMELESSUNIONNAME2
#define __C89_NAMELESSSTRUCTNAME
#define __C89_NAMELESSSTRUCTNAME1
#define __C89_NAMELESSSTRUCTNAME2

/* ------------------------------------------------- enum flag operators
 *
 * winnt.h gives every [Flags] enumeration the full set of bitwise operators
 * through this macro.  DXVK 3 relies on it heavily (88 translation units),
 * and the DirectX headers emit it next to the flag enums. */
#ifndef DEFINE_ENUM_FLAG_OPERATORS
#ifdef __cplusplus
#define DEFINE_ENUM_FLAG_OPERATORS(ENUMTYPE)                                     \
extern "C++" {                                                                   \
inline constexpr ENUMTYPE operator|(ENUMTYPE a, ENUMTYPE b) noexcept {            \
    return ENUMTYPE(((::std::underlying_type_t<ENUMTYPE>)a) |                    \
                    ((::std::underlying_type_t<ENUMTYPE>)b)); }                  \
inline constexpr ENUMTYPE operator&(ENUMTYPE a, ENUMTYPE b) noexcept {            \
    return ENUMTYPE(((::std::underlying_type_t<ENUMTYPE>)a) &                    \
                    ((::std::underlying_type_t<ENUMTYPE>)b)); }                  \
inline constexpr ENUMTYPE operator^(ENUMTYPE a, ENUMTYPE b) noexcept {            \
    return ENUMTYPE(((::std::underlying_type_t<ENUMTYPE>)a) ^                    \
                    ((::std::underlying_type_t<ENUMTYPE>)b)); }                  \
inline constexpr ENUMTYPE operator~(ENUMTYPE a) noexcept {                        \
    return ENUMTYPE(~((::std::underlying_type_t<ENUMTYPE>)a)); }                 \
inline ENUMTYPE &operator|=(ENUMTYPE &a, ENUMTYPE b) noexcept {                   \
    return a = (a | b); }                                                        \
inline ENUMTYPE &operator&=(ENUMTYPE &a, ENUMTYPE b) noexcept {                   \
    return a = (a & b); }                                                        \
inline ENUMTYPE &operator^=(ENUMTYPE &a, ENUMTYPE b) noexcept {                   \
    return a = (a ^ b); }                                                        \
}
#else
#define DEFINE_ENUM_FLAG_OPERATORS(ENUMTYPE)
#endif
#endif

/* --------------------------------------------------------- unknwn bits
 *
 * IUnknown and IClassFactory are defined in <unknwn.h>, which DXVK includes
 * explicitly.  Nothing is forward-declared here: a forward `typedef struct
 * IUnknown IUnknown;` would collide with the real definition in every
 * translation unit that pulls both in. */

/* ---------------------------------------------------------- sync types
 *
 * These hold the pthread primitive *by value* rather than a pointer to a
 * heap object.  Two reasons:
 *
 *   - DXVK declares them as members and initialises some statically
 *     (`SRWLOCK m_lock = SRWLOCK_INIT`), so a lazily created impl would need
 *     a lock in order to create the lock - a real race, not a theoretical one.
 *   - They are only ever addressed, never copied, so a fat struct costs
 *     nothing.
 *
 * Windows critical sections are recursive and so are these; SRW locks are
 * not, and neither is the mutex backing them.
 */
typedef struct {
    pthread_mutex_t mtx;
} CRITICAL_SECTION;
typedef CRITICAL_SECTION *LPCRITICAL_SECTION;
typedef CRITICAL_SECTION *PCRITICAL_SECTION;

typedef struct {
    pthread_cond_t cond;
} CONDITION_VARIABLE;
typedef CONDITION_VARIABLE *PCONDITION_VARIABLE;

typedef struct {
    pthread_mutex_t mtx;
} SRWLOCK, *PSRWLOCK;

#define SRWLOCK_INIT { PTHREAD_MUTEX_INITIALIZER }
#define CONDITION_VARIABLE_INIT { PTHREAD_COND_INITIALIZER }

/* GDI regions.  DXVK only carries RGNDATA pointers through its surface
 * bookkeeping; the layout has to match for the cast to be safe. */
typedef struct _RGNDATAHEADER {
    DWORD dwSize;
    DWORD iType;
    DWORD nCount;
    DWORD nRgnSize;
    RECT  rcBound;
} RGNDATAHEADER, *PRGNDATAHEADER;

typedef struct _RGNDATA {
    RGNDATAHEADER rdh;
    char Buffer[1];
} RGNDATA, *PRGNDATA;

#define RDH_RECTANGLES 1u

/* A handful of GDI value types that appear in the DirectX surface signatures
 * DXVK implements.  None of them is ever dereferenced by DXVK itself. */
typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER, *LPBITMAPINFOHEADER;

typedef struct tagRGBQUAD {
    BYTE rgbBlue;
    BYTE rgbGreen;
    BYTE rgbRed;
    BYTE rgbReserved;
} RGBQUAD, *LPRGBQUAD;

typedef struct tagPALETTEENTRY {
    BYTE peRed;
    BYTE peGreen;
    BYTE peBlue;
    BYTE peFlags;
} PALETTEENTRY, *PPALETTEENTRY, *LPPALETTEENTRY;

typedef struct tagRGBTRIPLE {
    BYTE rgbtBlue;
    BYTE rgbtGreen;
    BYTE rgbtRed;
} RGBTRIPLE;

typedef struct tagBITMAP {
    LONG  bmType;
    LONG  bmWidth;
    LONG  bmHeight;
    LONG  bmWidthBytes;
    WORD  bmPlanes;
    WORD  bmBitsPixel;
    void *bmBits;
} BITMAP, *PBITMAP, *NPBITMAP, *LPBITMAP;

typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD          bmiColors[1];
} BITMAPINFO, *PBITMAPINFO, *LPBITMAPINFO;

typedef void *HBITMAP;
typedef void *HPALETTE;
typedef void *HFONT;

#define DIB_RGB_COLORS 0
#define DIB_PAL_COLORS 1

/* ----------------------------------------------------------- inline bits */

static inline void *win32_InterlockedExchangePointer(void *volatile *p, void *v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}
static inline void *win32_InterlockedCompareExchangePointer(void *volatile *p,
                                                            void *v, void *c)
{
    __atomic_compare_exchange_n(p, &c, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}
static inline LONG win32_InterlockedCompareExchange(volatile LONG *p, LONG v, LONG c)
{
    __atomic_compare_exchange_n(p, &c, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}
static inline LONG win32_InterlockedExchangeAdd(volatile LONG *p, LONG v)
{
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}
static inline LONG win32_InterlockedIncrement(volatile LONG *p)
{
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}
static inline LONG win32_InterlockedDecrement(volatile LONG *p)
{
    return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
}
static inline LONG win32_InterlockedExchange(volatile LONG *p, LONG v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

#ifndef InterlockedIncrement
#define InterlockedIncrement(p) win32_InterlockedIncrement((volatile LONG *)(p))
#endif
#ifndef InterlockedDecrement
#define InterlockedDecrement(p) win32_InterlockedDecrement((volatile LONG *)(p))
#endif
#ifndef InterlockedExchange
#define InterlockedExchange(p, v) win32_InterlockedExchange((volatile LONG *)(p), (LONG)(v))
#endif
#ifndef InterlockedExchangeAdd
#define InterlockedExchangeAdd(p, v) win32_InterlockedExchangeAdd((volatile LONG *)(p), (LONG)(v))
#endif
#ifndef InterlockedCompareExchange
#define InterlockedCompareExchange(p, v, c) \
    win32_InterlockedCompareExchange((volatile LONG *)(p), (LONG)(v), (LONG)(c))
#endif
#ifndef InterlockedExchangePointer
#define InterlockedExchangePointer(p, v) win32_InterlockedExchangePointer((void *volatile *)(p), (void *)(v))
#endif
#ifndef InterlockedCompareExchangePointer
#define InterlockedCompareExchangePointer(p, v, c) \
    win32_InterlockedCompareExchangePointer((void *volatile *)(p), (void *)(v), (void *)(c))
#endif

#define DebugBreak() __builtin_trap()
#define __debugbreak() __builtin_trap()

/* --------------------------------------------------------- Win32 functions
 *
 * Implemented in runtime/android/dxvk_win32.c so the semantics live in one
 * place.  Every one of these has a real POSIX body, not a stub: DXVK's
 * synchronisation and its WSI backend both depend on them actually working. */

/* __uuidof is an MSVC operator with no clang equivalent for a non-MS target.
 *
 * The obvious emulation - `#define __uuidof(t) (IID_##t)` - covers the 300-odd
 * plain uses but breaks the ~20 that say `__uuidof(Base)` inside a template,
 * because the token paste happens where the template is *defined*, producing
 * an identifier `IID_Base` that never exists.  MinGW solves this with a
 * function template specialised per interface, which defers the lookup to
 * instantiation and therefore works for dependent types too.  That is the
 * scheme below; `__CRT_UUID_DECL` is what the Wine headers emit next to every
 * `DEFINE_GUID`, so the specialisations appear for free. */
#ifdef __cplusplus

template<typename T>
const GUID &__mingw_uuidof();

#define __CRT_UUID_DECL(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    extern "C++" {                                                        \
    template<> inline const GUID &__mingw_uuidof<type>() {                 \
        static const GUID __uuid_inst = WC3_GUID_INIT(                     \
            l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8);                    \
        return __uuid_inst;                                                \
    }                                                                      \
    template<> inline const GUID &__mingw_uuidof<type *>() {                \
        return __mingw_uuidof<type>();                                     \
    }                                                                      \
    }

#ifndef __uuidof
#define __uuidof(type) __mingw_uuidof<__typeof(type)>()
#endif

#else  /* !__cplusplus */

#define __CRT_UUID_DECL(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)

#endif

#ifdef __cplusplus
extern "C" {
#endif

/* memory */
void  *VirtualAlloc(void *addr, size_t size, DWORD type, DWORD protect);
BOOL   VirtualFree(void *addr, size_t size, DWORD type);
BOOL   VirtualProtect(void *addr, size_t size, DWORD newProt, DWORD *oldProt);
void   GetSystemInfo(SYSTEM_INFO *si);
void   GetNativeSystemInfo(SYSTEM_INFO *si);

/* Address-space walk.  DXVK itself never asks, but hle_d3d9_dxvk.cpp uses it
 * to find a free low region for the 32-bit staging arena, which is the one
 * thing in this port that genuinely needs to know what is mapped where. */
typedef struct _MEMORY_BASIC_INFORMATION {
    void  *BaseAddress;
    void  *AllocationBase;
    DWORD  AllocationProtect;
    size_t RegionSize;
    DWORD  State;
    DWORD  Protect;
    DWORD  Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

size_t VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, size_t len);

#define MEM_COMMIT  0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_RELEASE 0x00008000u
#define MEM_FREE    0x00010000u
#define PAGE_NOACCESS  0x01u
#define PAGE_READONLY  0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE_READWRITE 0x40u

/* time */
void      Sleep(DWORD ms);
DWORD     GetTickCount(void);
ULONGLONG GetTickCount64(void);
BOOL      QueryPerformanceCounter(LARGE_INTEGER *v);
BOOL      QueryPerformanceFrequency(LARGE_INTEGER *v);

/* threads */
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
BOOL  CreateDirectoryA(LPCSTR path, void *sa);
BOOL  CreateDirectoryW(LPCWSTR path, void *sa);
HANDLE CreateThread(void *sa, size_t stack, DWORD (__stdcall *fn)(void *),
                    void *arg, DWORD flags, DWORD *tid);
DWORD  WaitForSingleObject(HANDLE h, DWORD ms);
DWORD  WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD  WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
BOOL   SwitchToThread(void);
DWORD  GetActiveProcessorCount(WORD group);
BOOL   SetThreadPriority(HANDLE thread, int priority);
int    GetThreadPriority(HANDLE thread);
HANDLE GetCurrentThread(void);

/* synchronisation */
HANDLE CreateEventA(void *sa, BOOL manualReset, BOOL initialState, LPCSTR name);
HANDLE CreateEventW(void *sa, BOOL manualReset, BOOL initialState, LPCWSTR name);
HANDLE CreateSemaphoreA(void *sa, LONG initial, LONG max, LPCSTR name);
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);
BOOL   ReleaseSemaphore(HANDLE h, LONG count, LONG *prev);
BOOL   CloseHandle(HANDLE h);

void InitializeCriticalSection(CRITICAL_SECTION *cs);
void EnterCriticalSection(CRITICAL_SECTION *cs);
void LeaveCriticalSection(CRITICAL_SECTION *cs);
BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs);
void DeleteCriticalSection(CRITICAL_SECTION *cs);

void InitializeConditionVariable(CONDITION_VARIABLE *cv);
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms);
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags);
void WakeConditionVariable(CONDITION_VARIABLE *cv);
void WakeAllConditionVariable(CONDITION_VARIABLE *cv);

#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1u

void InitializeSRWLock(SRWLOCK *l);
void AcquireSRWLockExclusive(SRWLOCK *l);
void ReleaseSRWLockExclusive(SRWLOCK *l);
void AcquireSRWLockShared(SRWLOCK *l);
void ReleaseSRWLockShared(SRWLOCK *l);
BOOL TryAcquireSRWLockExclusive(SRWLOCK *l);
BOOL TryAcquireSRWLockShared(SRWLOCK *l);

/* TLS */
DWORD TlsAlloc(void);
BOOL  TlsFree(DWORD idx);
void *TlsGetValue(DWORD idx);
BOOL  TlsSetValue(DWORD idx, void *v);

/* errors */
DWORD GetLastError(void);
void  SetLastError(DWORD err);

/* modules */
HMODULE GetModuleHandleA(LPCSTR name);
HMODULE GetModuleHandleW(LPCWSTR name);
/* The Windows SDK makes GetModuleHandle an alias of the A form unless UNICODE
 * is defined, and DXVK 1.10.3 relies on that. */
#ifndef UNICODE
#define GetModuleHandle GetModuleHandleA
#endif

/* LUIDs: DXVK asks for one per adapter to tag it.  A monotonic counter is a
 * valid unique id here; nothing compares it against a driver's. */

/* monitors.  The EX forms flatten MONITORINFO's fields rather than nesting
 * them, because the Windows struct does and DXVK reads mi.cbSize directly on
 * the EX type. */
typedef struct tagMONITORINFO {
    DWORD cbSize;
    RECT  rcMonitor;
    RECT  rcWork;
    DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;

typedef struct tagMONITORINFOEXA {
    DWORD cbSize;
    RECT  rcMonitor;
    RECT  rcWork;
    DWORD dwFlags;
    CHAR  szDevice[32];
} MONITORINFOEXA, *LPMONITORINFOEXA;

typedef struct tagMONITORINFOEXW {
    DWORD cbSize;
    RECT  rcMonitor;
    RECT  rcWork;
    DWORD dwFlags;
    WCHAR szDevice[32];
} MONITORINFOEXW, *LPMONITORINFOEXW;

#ifndef UNICODE
typedef MONITORINFOEXA MONITORINFOEX;
typedef LPMONITORINFOEXA LPMONITORINFOEX;
#else
typedef MONITORINFOEXW MONITORINFOEX;
typedef LPMONITORINFOEXW LPMONITORINFOEX;
#endif

#define MONITORINFOF_PRIMARY 0x00000001u
#define MONITOR_DEFAULTTONULL    0u
#define MONITOR_DEFAULTTOPRIMARY 1u
#define MONITOR_DEFAULTTONEAREST 2u

HMONITOR MonitorFromPoint(POINT pt, DWORD flags);
HMONITOR MonitorFromRect(const RECT *rc, DWORD flags);
HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags);
BOOL     GetMonitorInfoA(HMONITOR m, LPMONITORINFO info);
BOOL     GetMonitorInfoW(HMONITOR m, LPMONITORINFO info);
#ifndef UNICODE
#define GetMonitorInfo GetMonitorInfoA
#endif
typedef BOOL (__stdcall *MONITORENUMPROC)(HMONITOR, HDC, RECT *, LPARAM);
BOOL EnumDisplayMonitors(HDC dc, const RECT *clip,
                         MONITORENUMPROC cb, LPARAM data);
BOOL EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW *dm);
BOOL EnumDisplaySettingsA(LPCSTR device, DWORD mode, DEVMODEA *dm);

/* ------------------------------------------------------- cursor and GDI
 *
 * D3D9 keeps a hardware cursor and hands it to the window system.  There is
 * no cursor on Android, so the implementations are benign no-ops, but the
 * declarations and ICONINFO layout must exist or d3d9_cursor.cpp cannot be
 * compiled at all. */
typedef struct _ICONINFO {
    BOOL    fIcon;
    DWORD   xHotspot;
    DWORD   yHotspot;
    HBITMAP hbmMask;
    HBITMAP hbmColor;
} ICONINFO, *PICONINFO;

/* GDI device contexts.  D3D9 asks for a compatible DC to describe its
 * offscreen surface format; there is no GDI here, so the handle is a token. */
HDC     CreateCompatibleDC(HDC dc);
HDC     GetDCEx(HWND wnd, HRGN clip, DWORD flags);
int     ReleaseDC(HWND wnd, HDC dc);
BOOL    ClientToScreen(HWND wnd, POINT *pt);
BOOL    BitBlt(HDC dst, int x, int y, int w, int h, HDC src,
               int sx, int sy, DWORD rop);
BOOL    StretchBlt(HDC dst, int dx, int dy, int dw, int dh, HDC src,
                   int sx, int sy, int sw, int sh, DWORD rop);
#define SRCCOPY 0x00CC0020u

/* --------------------------------------------------------- file access
 *
 * DXVK 3 implements dxvk::io on top of the Win32 file API, so these need
 * real POSIX bodies rather than stubs - see runtime/android/dxvk_win32.c. */
#ifndef CREATE_NEW
#define CREATE_NEW          1u
#define CREATE_ALWAYS       2u
#define OPEN_EXISTING       3u
#define OPEN_ALWAYS         4u
#define TRUNCATE_EXISTING   5u
#endif
#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL  0x00000080u
#endif

#define FILE_SHARE_READ     0x00000001u
#define FILE_SHARE_WRITE    0x00000002u
#define FILE_SHARE_DELETE   0x00000004u

#define FILE_BEGIN          0u
#define FILE_CURRENT        1u
#define FILE_END            2u

#define SEC_FILE            0x00800000u
#define SEC_IMAGE           0x01000000u
#define SEC_RESERVE         0x04000000u
#define SEC_COMMIT          0x08000000u
#define SEC_NOCACHE         0x10000000u

BOOL    ReadFile(HANDLE file, void *buf, DWORD count, DWORD *read, void *ov);
BOOL    WriteFile(HANDLE file, const void *buf, DWORD count, DWORD *written,
                  void *ov);
BOOL    FlushFileBuffers(HANDLE file);
BOOL    GetFileSizeEx(HANDLE file, LARGE_INTEGER *size);
BOOL    SetFilePointerEx(HANDLE file, LARGE_INTEGER distance,
                         LARGE_INTEGER *newPos, DWORD method);
HANDLE  CreateFileMappingA(HANDLE file, void *sa, DWORD protect,
                           DWORD maxHigh, DWORD maxLow, LPCSTR name);
HANDLE  OpenFileMappingA(DWORD access, BOOL inherit, LPCSTR name);
void   *MapViewOfFile(HANDLE map, DWORD access, DWORD offHigh, DWORD offLow,
                      size_t viewSize);
BOOL    UnmapViewOfFile(const void *base);
BOOL    DeleteDC(HDC dc);
BOOL    IsWindowUnicode(HWND wnd);
HMODULE GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *result);
HMODULE GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *result);
#ifndef UNICODE
#define GetModuleHandleEx GetModuleHandleExA
#else
#define GetModuleHandleEx GetModuleHandleExW
#endif
BOOL    SetProcessDPIAware(void);
BOOL    SetProcessDpiAwarenessContext(void *ctx);
BOOL    GetClientRect(HWND wnd, RECT *rc);
LRESULT DefWindowProcA(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT DefWindowProcW(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
#ifndef UNICODE
#define DefWindowProc DefWindowProcA
#else
#define DefWindowProc DefWindowProcW
#endif
BOOL    GetWindowRect(HWND wnd, RECT *rc);
LSTATUS RegNotifyChangeKeyValue(HKEY key, BOOL subtree, DWORD filter,
                                HANDLE event, BOOL async);

/* Device and file handles.  Used only by the shared-GPU-resource path, which
 * has no Android equivalent: both calls fail and the caller degrades. */
HANDLE  CreateFileA(LPCSTR name, DWORD access, DWORD share, void *sa,
                    DWORD disposition, DWORD flags, HANDLE tmpl);
HANDLE  CreateFileW(LPCWSTR name, DWORD access, DWORD share, void *sa,
                    DWORD disposition, DWORD flags, HANDLE tmpl);
#ifndef UNICODE
#define CreateFile CreateFileA
#else
#define CreateFile CreateFileW
#endif
DWORD   GetFileAttributesA(LPCSTR name);
BOOL    DeleteFileA(LPCSTR name);
BOOL    DeviceIoControl(HANDLE file, DWORD code, void *inBuf, DWORD inSize,
                        void *outBuf, DWORD outSize, DWORD *returned,
                        void *overlapped);

/* Display devices.  D3D9 enumerates adapters through these. */
#define DISPLAY_DEVICE_ATTACHED_TO_DESKTOP 0x00000001u
#define DISPLAY_DEVICE_MIRRORING_DRIVER    0x00000008u
#define DISPLAY_DEVICE_PRIMARY_DEVICE      0x00000004u

typedef struct _DISPLAY_DEVICEA {
    DWORD cb;
    CHAR  DeviceName[32];
    CHAR  DeviceString[128];
    DWORD StateFlags;
    CHAR  DeviceID[128];
    CHAR  DeviceKey[128];
} DISPLAY_DEVICEA, *PDISPLAY_DEVICEA, *LPDISPLAY_DEVICEA;

typedef struct _DISPLAY_DEVICEW {
    DWORD cb;
    WCHAR DeviceName[32];
    WCHAR DeviceString[128];
    DWORD StateFlags;
    WCHAR DeviceID[128];
    WCHAR DeviceKey[128];
} DISPLAY_DEVICEW, *PDISPLAY_DEVICEW, *LPDISPLAY_DEVICEW;

BOOL EnumDisplayDevicesA(LPCSTR device, DWORD devNum,
                         DISPLAY_DEVICEA *info, DWORD flags);
BOOL EnumDisplayDevicesW(LPCWSTR device, DWORD devNum,
                         DISPLAY_DEVICEW *info, DWORD flags);
#ifndef UNICODE
#define EnumDisplayDevices EnumDisplayDevicesA
#define DISPLAY_DEVICE     DISPLAY_DEVICEA
#else
#define EnumDisplayDevices EnumDisplayDevicesW
#define DISPLAY_DEVICE     DISPLAY_DEVICEW
#endif

/* Registry.  Only OpenVR's runtime detection reads it, and there is no
 * registry on Android, so every key lookup simply fails. */
#define HKEY_CURRENT_USER  ((HKEY)(uintptr_t)0x80000001u)
#define HKEY_LOCAL_MACHINE ((HKEY)(uintptr_t)0x80000002u)
#define HKEY_CLASSES_ROOT  ((HKEY)(uintptr_t)0x80000000u)

#define KEY_READ        0x00020019u
#define KEY_WRITE       0x00020006u
#define KEY_ALL_ACCESS  0x000F003Fu

#define REG_SZ          1u
#define REG_EXPAND_SZ   2u
#define REG_BINARY      3u
#define REG_DWORD       4u
#define REG_MULTI_SZ    7u
#define REG_NOTIFY_CHANGE_NAME       0x00000001u
#define REG_NOTIFY_CHANGE_ATTRIBUTES 0x00000002u
#define REG_NOTIFY_CHANGE_LAST_SET   0x00000004u
#define REG_NOTIFY_CHANGE_SECURITY   0x00000008u

LSTATUS RegOpenKeyExA(HKEY key, LPCSTR sub, DWORD reserved,
                      DWORD access, HKEY *result);
LSTATUS RegQueryValueExA(HKEY key, LPCSTR name, DWORD *reserved,
                         DWORD *type, BYTE *data, DWORD *size);
LSTATUS RegCloseKey(HKEY key);

/* Display mode changes. */
#define CDS_UPDATEREGISTRY  0x00000001u
#define CDS_TEST            0x00000002u
#define CDS_FULLSCREEN      0x00000004u
#define DM_INTERLACED            0x00000002u
#define DM_DISPLAYFLAGS          0x00200000u
#define DM_DISPLAYORIENTATION    0x00000080u
#define DM_BITSPERPEL            0x00040000u
#define DM_PELSWIDTH             0x00080000u
#define DM_PELSHEIGHT            0x00100000u
#define DM_DISPLAYFREQUENCY      0x00400000u
#define DISP_CHANGE_SUCCESSFUL   0
#define DISP_CHANGE_BADMODE     -2
#define DISP_CHANGE_FAILED      -1

LONG ChangeDisplaySettingsExA(LPCSTR device, DEVMODEA *mode, HWND wnd,
                              DWORD flags, void *param);
LONG ChangeDisplaySettingsExW(LPCWSTR device, DEVMODEW *mode, HWND wnd,
                              DWORD flags, void *param);
#ifndef UNICODE
#define ChangeDisplaySettingsEx ChangeDisplaySettingsExA
#else
#define ChangeDisplaySettingsEx ChangeDisplaySettingsExW
#endif

BOOL    GetCursorPos(LPPOINT pt);
BOOL    SetCursorPos(int x, int y);
HCURSOR SetCursor(HCURSOR cursor);
HBITMAP CreateBitmap(int w, int h, UINT planes, UINT bitsPerPixel,
                     const void *bits);
HCURSOR CreateIconIndirect(PICONINFO info);
BOOL    DestroyCursor(HCURSOR cursor);
BOOL    DeleteObject(HGDIOBJ obj);

/* Window long accessors.  DXVK's Win32 WSI reads the HINSTANCE off the
 * window; the pointers are 64-bit even on the 32-bit ABI, hence the separate
 * Ptr forms. */
LONG_PTR GetWindowLongPtrA(HWND wnd, int index);
LONG_PTR GetWindowLongPtrW(HWND wnd, int index);
LONG_PTR SetWindowLongPtrA(HWND wnd, int index, LONG_PTR value);
LONG_PTR SetWindowLongPtrW(HWND wnd, int index, LONG_PTR value);
#ifndef UNICODE
#define GetWindowLongPtr GetWindowLongPtrA
#define SetWindowLongPtr SetWindowLongPtrA
#else
#define GetWindowLongPtr GetWindowLongPtrW
#define SetWindowLongPtr SetWindowLongPtrW
#endif

DWORD   GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n);
DWORD   GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD n);
HMODULE LoadLibraryA(LPCSTR name);
HMODULE LoadLibraryW(LPCWSTR name);
#ifndef UNICODE
#define LoadLibrary LoadLibraryA
#endif
void   *GetProcAddress(HMODULE m, LPCSTR name);
BOOL    FreeLibrary(HMODULE m);

/* Window placement.  The swapchain resizes its window on reset and on
 * fullscreen transitions; on Android there is no window to move, so the
 * flags only have to exist. */
/* DCX_* flags select which device contexts GetDCEx may return. */
#define DCX_WINDOW           0x00000001u
#define DCX_CACHE            0x00000002u
#define DCX_PARENTCLIP       0x00000020u
#define DCX_CLIPSIBLINGS     0x00000010u
#define DCX_CLIPCHILDREN     0x00000008u

#define SWP_NOSIZE        0x0001u
#define SWP_NOMOVE        0x0002u
#define SWP_NOZORDER      0x0004u
#define SWP_NOREDRAW      0x0008u
#define SWP_NOACTIVATE    0x0010u
#define SWP_FRAMECHANGED  0x0020u
#define SWP_SHOWWINDOW    0x0040u
#define SWP_HIDEWINDOW    0x0080u
#define SWP_NOOWNERZORDER 0x0200u

#define HWND_TOP       ((HWND)0)
#define HWND_BOTTOM    ((HWND)1)
#define HWND_TOPMOST   ((HWND)-1)
#define HWND_NOTOPMOST ((HWND)-2)

#define SW_SHOW      5
#define SW_SHOWNOACTIVATE 4
#define SW_MINIMIZE  6
#define SW_RESTORE   9

BOOL    SetWindowPos(HWND wnd, HWND insertAfter, int x, int y, int cx, int cy,
                     UINT flags);
BOOL    ShowWindow(HWND wnd, int cmd);
BOOL    SetForegroundWindow(HWND wnd);

/* Window styles.  The swapchain compares the style bits to decide whether it
 * owns a fullscreen or a windowed swapchain. */
#define WS_OVERLAPPED       0x00000000u
#define WS_POPUP            0x80000000u
#define WS_CHILD            0x40000000u
#define WS_VISIBLE          0x10000000u
#define WS_CAPTION          0x00C00000u
#define WS_BORDER           0x00800000u
#define WS_THICKFRAME       0x00040000u
#define WS_SYSMENU          0x00080000u
#define WS_MINIMIZEBOX      0x00020000u
#define WS_MAXIMIZEBOX      0x00010000u
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | \
                             WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

#define WS_EX_TOPMOST       0x00000008u
#define WS_EX_WINDOWEDGE    0x00000100u
#define WS_EX_CLIENTEDGE    0x00000200u
#define WS_EX_OVERLAPPEDWINDOW (WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE)

/* Window queries and message posting. */
BOOL    IsWindow(HWND wnd);
BOOL    IsWindowVisible(HWND wnd);
BOOL    IsIconic(HWND wnd);
BOOL    IsZoomed(HWND wnd);
BOOL    PostMessageA(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
BOOL    PostMessageW(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CallWindowProcA(WNDPROC proc, HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CallWindowProcW(WNDPROC proc, HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
LONG    GetWindowLongA(HWND wnd, int index);
LONG    GetWindowLongW(HWND wnd, int index);
LONG    SetWindowLongA(HWND wnd, int index, LONG value);
LONG    SetWindowLongW(HWND wnd, int index, LONG value);

#ifndef UNICODE
#define PostMessage     PostMessageA
#define CallWindowProc  CallWindowProcA
#define GetWindowLong   GetWindowLongA
#define SetWindowLong   SetWindowLongA
#else
#define PostMessage     PostMessageW
#define CallWindowProc  CallWindowProcW
#define GetWindowLong   GetWindowLongW
#define SetWindowLong   SetWindowLongW
#endif

/* Window messages.  DXVK's swapchain hooks exactly these four. */
#define WM_DESTROY      0x0002u
#define WM_SIZE         0x0005u
#define WM_ACTIVATEAPP  0x001Cu
#define WM_NCCALCSIZE   0x0083u
#define WM_DPICHANGED   0x02E0u

/* Window long indices.  DXVK's WSI reads the instance handle off the window. */
#define GWLP_WNDPROC    (-4)
#define GET_MODULE_HANDLE_EX_FLAG_PIN               0x00000001u
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002u
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS      0x00000004u
#define GWLP_HINSTANCE  (-6)
#define GWLP_USERDATA   (-21)
#define GWL_STYLE       (-16)
#define GWL_EXSTYLE     (-20)

/* debug output */
void OutputDebugStringA(LPCSTR s);
void OutputDebugStringW(LPCWSTR s);

/* string conversion (wchar is 16-bit here, so these are real conversions) */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srcLen,
                        LPWSTR dst, int dstLen);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int srcLen,
                        LPSTR dst, int dstLen, LPCSTR defChar, BOOL *usedDef);
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD n);
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n);

#ifdef __cplusplus
}
#endif

/* CP_ACP and friends: DXVK converts with the ANSI code page, and on Android
 * that is UTF-8. */
#define CP_ACP   0u
#define CP_UTF8  65001u
#define WC_ERR_INVALID_CHARS 0x00000080u
#define WC_NO_BEST_FIT_CHARS 0x00000400u

/* --------------------------------------------------- misc inline helpers */

static inline void win32_SleepInline(DWORD ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
}

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(p) ((void)(p))
#endif
#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define DLLEXPORT
#define DLLIMPORT
#define WINAPI_FAMILY_PARTITION(x) 1

/* Wide-string literals: the build uses -fshort-wchar, so L"..." is 16-bit. */
#ifndef TEXT
#define TEXT(x) x
#endif
#ifndef _T
#define _T(x) x
#endif

#endif /* WC3_ANDROID_DXVK_WINDOWS_H */
