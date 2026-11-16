/*
 * ole32_hle.h - the guest-visible DirectShow facade used by 1.27 movies.
 *
 * The game creates a FilterGraph through ole32!CoCreateInstance and drives
 * only the small COM surface it needs: IGraphBuilder::RenderFile,
 * IMediaEventEx (notification + event drain), IVideoWindow (owner/style/rect)
 * and IMediaControl (Run/Stop).  This module supplies those objects and hands
 * the actual file to Wc3Platform.video, the Android host MoviePlayer (Media3
 * audio/clock, FFmpeg video).
 *
 * The completion path is deliberately the DirectShow one: the Java player
 * reports end-of-stream, this module stores EC_COMPLETE/EC_USERABORT/
 * EC_ERRORABORT and posts WMU_CALL to the window registered with
 * SetNotifyWindow.  The game's own WndProc then calls GetEvent and posts the
 * synthetic WM_LBUTTONUP that leaves the movie.
 */
#ifndef WC3_HLE_OLE32_H
#define WC3_HLE_OLE32_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install the ole32 imports and the COM method rows.  The real imports of
 * game.dll (CoInitialize, CoCreateInstance, CoUninitialize) are replaced in
 * place; the COM methods are appended to the registry and their thunks are
 * placed in guest-owned vtables.  Returns 0 on success. */
int wc3_hle_ole32_install(void);

/* Java movie callback.  status: 0 natural end, 2 user abort, other error.
 * Safe to call from the UI thread; the guest notification itself is queued
 * through the lock-free message mailbox. */
void wc3_hle_ole32_video_finished(int status);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_OLE32_H */
