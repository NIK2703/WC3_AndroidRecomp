package wc3.recomp;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import androidx.annotation.Nullable;
import androidx.media3.common.Format;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.common.Tracks;
import androidx.media3.exoplayer.DecoderCounters;
import androidx.media3.exoplayer.DecoderReuseEvaluation;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.exoplayer.analytics.AnalyticsListener;
import androidx.media3.exoplayer.source.LoadEventInfo;
import androidx.media3.exoplayer.source.MediaLoadData;

import java.io.File;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Debug-only trace for the Media3/Media3Avi movie path.
 *
 * <p>The class lives in the debug source set; {@link MovieDiagnosticsBridge}
 * is the release-safe no-op that loads it only when the debug resource says
 * so.  Media3 owns only AVI audio and the playback clock; the video track is
 * disabled.  This trace therefore reports audio underruns, surface
 * lifecycle, player state and position, while {@code movie_ffmpeg.c} reports
 * FFmpeg decode order, frame drops and render timing on the same
 * {@code wc3movie} tag.
 *
 * <p>Run logs:
 * <pre>
 *   adb logcat -s wc3movie
 * </pre>
 */
@SuppressWarnings("UnstableApiUsage")
public final class MovieDiagnostics implements AnalyticsListener, Runnable,
        SurfaceHolder.Callback {
    private static final String TAG = "wc3movie";
    private static final long REPORT_INTERVAL_MS = 1000L;
    private static volatile boolean sCodecsDumped;

    private final ExoPlayer player;
    private final SurfaceView surfaceView;
    private final String path;
    private final AviFile avi;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final long startedMs = SystemClock.elapsedRealtime();

    private final AtomicInteger audioUnderruns = new AtomicInteger();
    private volatile boolean stopped;

    private MovieDiagnostics(ExoPlayer player, String path, AviFile avi,
                             SurfaceView surfaceView) {
        this.player = player;
        this.path = path;
        this.avi = avi;
        this.surfaceView = surfaceView;
    }

    public static Object attach(ExoPlayer player, String path, AviFile avi,
                                SurfaceView surfaceView) {
        MovieDiagnostics trace =
                new MovieDiagnostics(player, path, avi, surfaceView);
        trace.start();
        return trace;
    }

    public static void detach(Object token) {
        if (token instanceof MovieDiagnostics) {
            ((MovieDiagnostics) token).stop();
        }
    }

    private void start() {
        File file = new File(path);
        Log.i(TAG, "attach path=" + path
                + " bytes=" + file.length()
                + " mtime=" + file.lastModified()
                + " avi=" + avi.width + "x" + avi.height
                + "@" + fps()
                + " surface=" + surfaceView.getWidth() + "x"
                + surfaceView.getHeight()
                + " holder=" + surfaceView.getHolder().getSurfaceFrame());
        dumpDeviceAndCodecs();
        player.addAnalyticsListener(this);
        surfaceView.getHolder().addCallback(this);
        handler.postDelayed(this, REPORT_INTERVAL_MS);
    }

    private void stop() {
        if (stopped) return;
        stopped = true;
        handler.removeCallbacks(this);
        try {
            player.removeAnalyticsListener(this);
        } catch (Throwable ignored) {
        }
        try {
            surfaceView.getHolder().removeCallback(this);
        } catch (Throwable ignored) {
        }
        Log.i(TAG, "detach t=" + elapsedMs() + "ms"
                + " state=" + stateName(player.getPlaybackState())
                + " underruns=" + audioUnderruns.get());
    }

    @Override
    public void run() {
        if (stopped) return;
        long now = SystemClock.elapsedRealtime();
        Log.i(TAG, "t=" + (now - startedMs) + "ms"
                + " state=" + stateName(player.getPlaybackState())
                + " playing=" + player.isPlaying()
                + " pos=" + player.getCurrentPosition() + "ms"
                + " buffered=" + player.getBufferedPosition() + "ms"
                + " surface=" + surfaceView.getWidth() + "x"
                + surfaceView.getHeight()
                + " underruns=" + audioUnderruns.get());
        handler.postDelayed(this, REPORT_INTERVAL_MS);
    }

    /* ---------------------------------------------------------------- */
    /* Surface lifecycle                                                */
    /* ---------------------------------------------------------------- */

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Log.i(TAG, "surfaceCreated frame=" + holder.getSurfaceFrame()
                + " valid=" + holder.getSurface().isValid());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width,
                               int height) {
        Log.i(TAG, "surfaceChanged format=" + format + " size=" + width + "x"
                + height + " frame=" + holder.getSurfaceFrame());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "surfaceDestroyed frame=" + holder.getSurfaceFrame());
    }

    /* ---------------------------------------------------------------- */
    /* Player state                                                     */
    /* ---------------------------------------------------------------- */

    @Override
    public void onPlaybackStateChanged(EventTime eventTime, int state) {
        Log.i(TAG, "playbackState=" + stateName(state)
                + " pos=" + eventTime.currentPlaybackPositionMs + "ms"
                + " buffered=" + eventTime.totalBufferedDurationMs + "ms");
    }

    @Override
    public void onIsPlayingChanged(EventTime eventTime, boolean isPlaying) {
        Log.i(TAG, "isPlaying=" + isPlaying
                + " pos=" + eventTime.currentPlaybackPositionMs + "ms");
    }

    @Override
    public void onPositionDiscontinuity(EventTime eventTime,
                                        Player.PositionInfo oldPosition,
                                        Player.PositionInfo newPosition,
                                        int reason) {
        Log.i(TAG, "positionDiscontinuity reason=" + reason
                + " old=" + oldPosition.positionMs
                + " new=" + newPosition.positionMs);
    }

    @Override
    public void onTracksChanged(EventTime eventTime, Tracks tracks) {
        for (Tracks.Group group : tracks.getGroups()) {
            for (int i = 0; i < group.length; i++) {
                Log.i(TAG, "track type=" + group.getType()
                        + " selected=" + group.isTrackSelected(i)
                        + " supported=" + group.isTrackSupported(i)
                        + " format=" + describe(group.getTrackFormat(i)));
            }
        }
    }

    @Override
    public void onPlayerError(EventTime eventTime, PlaybackException error) {
        Log.e(TAG, "playerError code=" + error.getErrorCodeName()
                + " pos=" + eventTime.currentPlaybackPositionMs + "ms"
                + " message=" + error.getMessage(), error);
    }

    @Override
    public void onLoadError(EventTime eventTime, LoadEventInfo loadEventInfo,
                            MediaLoadData mediaLoadData, java.io.IOException error,
                            boolean wasCanceled) {
        Log.e(TAG, "loadError canceled=" + wasCanceled
                + " type=" + mediaLoadData.dataType
                + " message=" + error.getMessage(), error);
    }

    /* Media3 video analytics are intentionally absent: the video track is
     * disabled and movie_ffmpeg.c reports decode, render and drop timing on
     * the same wc3movie tag. */

    /* ---------------------------------------------------------------- */
    /* Audio                                                            */
    /* ---------------------------------------------------------------- */

    @Override
    public void onAudioEnabled(EventTime eventTime, DecoderCounters counters) {
        Log.i(TAG, "audioEnabled counters=" + counters);
    }

    @Override
    public void onAudioDecoderInitialized(EventTime eventTime,
                                           String decoderName,
                                           long initializedTimestampMs,
                                           long initializationDurationMs) {
        Log.i(TAG, "audioDecoder name=" + decoderName
                + " init=" + initializationDurationMs + "ms"
                + " at=" + initializedTimestampMs + "ms");
    }

    @Override
    public void onAudioInputFormatChanged(EventTime eventTime, Format format,
                                          @Nullable DecoderReuseEvaluation reuse) {
        Log.i(TAG, "audioFormat format=" + describe(format)
                + " reuse=" + (reuse == null ? "null" : reuse.result));
    }

    @Override
    public void onAudioUnderrun(EventTime eventTime, int bufferSize,
                                long bufferSizeMs,
                                long elapsedSinceLastFeedMs) {
        int total = audioUnderruns.incrementAndGet();
        Log.w(TAG, "audioUnderrun total=" + total
                + " bufferSize=" + bufferSize
                + " bufferMs=" + bufferSizeMs
                + " sinceFeed=" + elapsedSinceLastFeedMs + "ms"
                + " pos=" + eventTime.currentPlaybackPositionMs + "ms");
    }

    @Override
    public void onAudioDisabled(EventTime eventTime, DecoderCounters counters) {
        Log.i(TAG, "audioDisabled counters=" + counters);
    }

    @Override
    public void onAudioSinkError(EventTime eventTime, Exception error) {
        Log.e(TAG, "audioSinkError message=" + error.getMessage(), error);
    }

    @Override
    public void onAudioCodecError(EventTime eventTime, Exception error) {
        Log.e(TAG, "audioCodecError message=" + error.getMessage(), error);
    }

    /* ---------------------------------------------------------------- */
    /* Helpers                                                          */
    /* ---------------------------------------------------------------- */

    private long elapsedMs() {
        return SystemClock.elapsedRealtime() - startedMs;
    }

    private float fps() {
        return avi.fpsDen > 0 ? avi.fpsNum / (float) avi.fpsDen : 0.0f;
    }

    private static String describe(Format format) {
        if (format == null) return "null";
        int init = format.initializationData == null
                ? 0 : format.initializationData.size();
        return Format.toLogString(format) + " initData=" + init
                + " maxInputSize=" + format.maxInputSize;
    }

    private static String stateName(int state) {
        switch (state) {
            case Player.STATE_IDLE:
                return "IDLE";
            case Player.STATE_BUFFERING:
                return "BUFFERING";
            case Player.STATE_READY:
                return "READY";
            case Player.STATE_ENDED:
                return "ENDED";
            default:
                return Integer.toString(state);
        }
    }

    private static void dumpDeviceAndCodecs() {
        if (sCodecsDumped) return;
        sCodecsDumped = true;
        Log.i(TAG, "device " + Build.MANUFACTURER + " " + Build.MODEL
                + " hardware=" + Build.HARDWARE
                + " sdk=" + Build.VERSION.SDK_INT);
        try {
            MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
            for (MediaCodecInfo info : list.getCodecInfos()) {
                for (String type : info.getSupportedTypes()) {
                    if (!"video/mp4v-es".equalsIgnoreCase(type)
                            && !"audio/mpeg".equalsIgnoreCase(type)) {
                        continue;
                    }
                    StringBuilder profiles = new StringBuilder();
                    try {
                        MediaCodecInfo.CodecCapabilities caps =
                                info.getCapabilitiesForType(type);
                        if (caps.profileLevels != null) {
                            for (MediaCodecInfo.CodecProfileLevel pl
                                    : caps.profileLevels) {
                                if (profiles.length() > 0) profiles.append(',');
                                profiles.append(pl.profile).append('/')
                                        .append(pl.level);
                            }
                        }
                    } catch (Throwable ignored) {
                    }
                    String kind = "?";
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        kind = "hw=" + info.isHardwareAccelerated()
                                + " sw=" + info.isSoftwareOnly();
                    }
                    Log.i(TAG, "codec type=" + type + " name=" + info.getName()
                            + " " + kind
                            + " profiles=" + profiles);
                }
            }
        } catch (Throwable error) {
            Log.w(TAG, "codecList failed: " + error.getMessage(), error);
        }
    }
}
