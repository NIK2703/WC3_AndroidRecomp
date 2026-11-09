/*
 * ws2_32_hle.h - winsock 1/2 imports over Wc3NetApi.
 *
 * Warcraft III 1.27 imports WS2_32 by ordinal from game.dll and WSOCK32 by
 * ordinal from war3.exe.  The two DLLs disagree at ordinals 10/11/12, so the
 * ordinal tables stay separate.  This module owns the guest SOCKET handle
 * table, fd_set/sockaddr/HOSTENT/ADDRINFO conversion and WinSock error codes;
 * runtime/platform/android/net_android.c owns the host socket calls.
 *
 * The module also supplies pseudo HMODULEs for ws2_32.dll and mswsock.dll.
 * The as-built LoadLibraryA path only knows its fixed system-module list, so
 * chaining that call is what makes the game's dynamic winsock probe succeed.
 */
#ifndef WC3_HLE_WIN32_WS2_32_H
#define WC3_HLE_WIN32_WS2_32_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install named and ordinal rows, then chain LoadLibraryA/GetProcAddress.
 * Must run after hle_android_start() populated the as-built table and before
 * the first guest thread.  Returns the number of rows installed, or -1 when
 * Wc3NetApi or a mandatory as-built row is missing. */
int wc3_hle_ws2_32_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_WIN32_WS2_32_H */
