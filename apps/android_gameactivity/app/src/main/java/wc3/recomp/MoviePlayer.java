package wc3.recomp;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import androidx.media3.common.AudioAttributes;
import androidx.media3.common.C;
import androidx.media3.common.MediaItem;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.exoplayer.source.DefaultMediaSourceFactory;

import com.homesoft.exo.extractor.AviExtractorsFactory;

import java.io.File;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Campaign movie player.
 *
 * <p>The game talks to a DirectShow facade in native code.  This class is the
 * host side of that facade.  Media3 with the Media3Avi extractor owns the AVI
 * audio samples and the audio clock; its video track is disabled.  The video
 * stream is MPEG-4 Part 2 Advanced Simple Profile with B-frames, so native
 * FFmpeg decodes it in display order and presents frames to the SurfaceView
 * through ANativeWindow.  The platform MediaCodec decoder on the target
 * device is Simple Profile only and cannot reorder these B-frames.
 *
 * <p>The movie layer is a separate SurfaceView above GameActivity's surface.
 * While the movie owns the screen, the D3D9 bridge skips guest presents, so
 * DXVK does not compete with the decoder for CPU, GPU and memory bandwidth.
 */
@SuppressWarnings("UnstableApiUsage")
final class MoviePlayer {
    /** Host callbacks; both run on the main thread. */
    interface Host {
        void setOverlayVisible(boolean visible);

        void onFinished(int status);
    }

    private final Activity activity;
    private final Host host;
    private final FrameLayout layer;
    private final SurfaceView surfaceView;
    private final AtomicInteger generation = new AtomicInteger();
    private final AtomicBoolean finished = new AtomicBoolean();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final MovieClock movieClock = new MovieClock();

    private ExoPlayer player;
    private Object diagnostics;
    private float movieFps;
    private String moviePath;
    private long videoHandle;
    private boolean videoStarted;
    private boolean videoStartFailed;
    private int videoGeneration;

    /**
     * Playback position kept fresh by the UI-thread ticker.  The native video
     * thread reads it through one cached JNI method while pacing a frame, so
     * it never touches ExoPlayer or the player's locks.
     */
    static final class MovieClock {
        private volatile long positionMs;

        long positionMs() {
            return positionMs;
        }

        void setPositionMs(long value) {
            positionMs = value;
        }
    }

    private final Runnable clockTick = new Runnable() {
        @Override
        public void run() {
            ExoPlayer exo = player;
            if (exo == null) return;
            movieClock.setPositionMs(Math.max(0L, exo.getCurrentPosition()));
            handler.postDelayed(this, 10L);
        }
    };

    private static native long nativeStartVideo(String path, Surface surface,
                                                MovieClock clock,
                                                long startPositionMs);

    private static native void nativeStopVideo(long handle);

    private static native void nativeClearVideoSurface(Surface surface);

    /**
     * SurfaceFlinger otherwise assumes the display refresh cadence for the
     * movie layer.  Telling it the actual 24 or 23.976 fps cadence lets a
     * 120 Hz panel use an exact 5:5 cadence instead of a 3:2 pattern.
     * ExoPlayer's video renderer is disabled, so this is the only frame-rate
     * hint the surface gets.
     */
    private final SurfaceHolder.Callback surfaceCallback =
            new SurfaceHolder.Callback() {
                @Override
                public void surfaceCreated(SurfaceHolder holder) {
                    applySurfaceFrameRate();
                    // A failed native start may have raced surface creation;
                    // a new surface is a fresh chance to start the video.
                    videoStartFailed = false;
                    maybeStartVideo();
                }

                @Override
                public void surfaceChanged(SurfaceHolder holder, int format,
                                           int width, int height) {
                    applySurfaceFrameRate();
                    videoStartFailed = false;
                    maybeStartVideo();
                }

                @Override
                public void surfaceDestroyed(SurfaceHolder holder) {
                    stopVideo();
                }
            };

    MoviePlayer(Activity activity, Host host) {
        this.activity = activity;
        this.host = host;

        FrameLayout frame = new FrameLayout(activity);
        frame.setBackgroundColor(Color.BLACK);

        surfaceView = new SurfaceView(activity);
        surfaceView.setZOrderOnTop(true);
        surfaceView.setFocusable(false);
        surfaceView.setFocusableInTouchMode(false);
        surfaceView.getHolder().setFormat(PixelFormat.RGBA_8888);
        surfaceView.getHolder().addCallback(surfaceCallback);

        frame.addView(surfaceView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        frame.setVisibility(View.GONE);
        activity.addContentView(frame, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        layer = frame;
    }

    /** Called from the DirectShow facade on the guest thread. */
    void play(final String path) {
        final int gen = generation.incrementAndGet();
        finished.set(false);

        // Stop the previous graph before parsing the next file.  The parse can
        // take a moment and must not leave the old movie running underneath.
        activity.runOnUiThread(() -> {
            if (gen == generation.get()) releasePlayer();
        });

        Thread worker = new Thread(() -> {
            final AviFile avi;
            try {
                avi = AviFile.parse(new File(path));
            } catch (Throwable ignored) {
                finish(gen, 1);
                return;
            }

            activity.runOnUiThread(() -> {
                if (gen != generation.get()) return;
                try {
                    sizeSurface(avi);
                    host.setOverlayVisible(false);
                    layer.setVisibility(View.VISIBLE);
                    startPlayer(path, avi, gen);
                } catch (Throwable ignored) {
                    finish(gen, 1);
                }
            });
        }, "wc3-movie");
        worker.setDaemon(true);
        worker.start();
    }

    /** Called when the guest stops the graph or the activity is destroyed. */
    void stop() {
        generation.incrementAndGet();
        activity.runOnUiThread(() -> {
            releasePlayer();
            layer.setVisibility(View.GONE);
            host.setOverlayVisible(true);
        });
    }

    /**
     * Complete the movie on the main thread.  The player is released and the
     * layer is hidden before the guest is told, so the next movie cannot start
     * against the old SurfaceView and the game starts only after the player has
     * been released and the FFmpeg decoder thread has been joined.
     */
    private void finish(int gen, int status) {
        if (gen != generation.get()) return;
        if (!finished.compareAndSet(false, true)) return;
        generation.incrementAndGet();

        activity.runOnUiThread(() -> {
            releasePlayer();
            layer.setVisibility(View.GONE);
            host.setOverlayVisible(true);
            host.onFinished(status);
        });
    }

    /**
     * Size the SurfaceView to the movie's aspect ratio inside the full-screen
     * layer, then make the layer visible.  The movie layer is still GONE here,
     * so its own width and height are zero; use its parent (the activity
     * content view), which has the same bounds as the game surface.
     * DisplayMetrics includes system-bar area that the game surface does not,
     * and using it would clip the movie on devices with a visible navigation
     * bar.
     */
    private void sizeSurface(AviFile avi) {
        if (avi.width <= 0 || avi.height <= 0) {
            throw new IllegalArgumentException("movie dimensions");
        }
        movieFps = avi.fpsDen > 0
                ? avi.fpsNum / (float) avi.fpsDen : 0.0f;

        DisplayMetrics dm = activity.getResources().getDisplayMetrics();
        View container = layer.getParent() instanceof View
                ? (View) layer.getParent() : null;
        int containerWidth = container != null && container.getWidth() > 0
                ? container.getWidth() : dm.widthPixels;
        int containerHeight = container != null && container.getHeight() > 0
                ? container.getHeight() : dm.heightPixels;
        if (containerWidth <= 0 || containerHeight <= 0) {
            throw new IllegalStateException("movie container");
        }

        double scale = Math.min(containerWidth / (double) avi.width,
                containerHeight / (double) avi.height);
        int width = Math.max(1, (int) Math.round(avi.width * scale));
        int height = Math.max(1, (int) Math.round(avi.height * scale));
        surfaceView.setLayoutParams(new FrameLayout.LayoutParams(
                width, height, Gravity.CENTER));
        applySurfaceFrameRate();
    }

    private void startPlayer(String path, AviFile avi, int gen) {
        if (gen != generation.get()) return;

        moviePath = path;
        videoGeneration = gen;
        videoStarted = false;
        videoStartFailed = false;
        videoHandle = 0;
        movieClock.setPositionMs(0);

        ExoPlayer exo = new ExoPlayer.Builder(activity)
                .setMediaSourceFactory(new DefaultMediaSourceFactory(
                        activity, new AviExtractorsFactory()))
                .build();
        player = exo;

        exo.setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(C.USAGE_MEDIA)
                .setContentType(C.AUDIO_CONTENT_TYPE_MOVIE)
                .build(), true);
        // The platform MPEG-4 decoder on the target device is Simple Profile
        // only.  Leave the video track to FFmpeg and keep Media3 responsible
        // for AVI audio and the playback clock.
        exo.setTrackSelectionParameters(
                exo.getTrackSelectionParameters().buildUpon()
                        .setTrackTypeDisabled(C.TRACK_TYPE_VIDEO, true)
                        .build());
        diagnostics = MovieDiagnosticsBridge.attach(
                activity, exo, path, avi, surfaceView);
        exo.addListener(new Player.Listener() {
            @Override
            public void onPlaybackStateChanged(int playbackState) {
                if (playbackState == Player.STATE_ENDED) {
                    finish(gen, 0);
                } else if (playbackState == Player.STATE_READY) {
                    maybeStartVideo();
                }
            }

            @Override
            public void onIsPlayingChanged(boolean isPlaying) {
                if (isPlaying) maybeStartVideo();
            }

            @Override
            public void onPlayerError(PlaybackException error) {
                finish(gen, 1);
            }
        });
        exo.setMediaItem(MediaItem.fromUri(
                Uri.fromFile(new File(path))));
        // Give FFmpeg a head start while ExoPlayer prepares and buffers the
        // audio.  The video thread renders the first frame and then waits for
        // the audio clock, so playback still starts in sync.
        maybeStartVideo();
        exo.prepare();
        exo.play();
        handler.removeCallbacks(clockTick);
        handler.post(clockTick);
    }

    /**
     * Start the FFmpeg video thread as soon as a player, a movie path and a
     * real surface exist.  It is called at pre-roll, from READY/isPlaying and
     * from surface creation because the player and the surface can arrive in
     * either order; once a thread is running the call is a no-op.  Before
     * prepare the playback position is 0, which is the right pre-roll clock.
     */
    private void maybeStartVideo() {
        ExoPlayer exo = player;
        if (exo == null || videoStarted || videoStartFailed) return;
        if (videoGeneration != generation.get()) return;
        Surface surface = surfaceView.getHolder().getSurface();
        if (surface == null || !surface.isValid()) return;
        String path = moviePath;
        if (path == null) return;

        long startMs = Math.max(0L, exo.getCurrentPosition());
        // The SurfaceView buffer queue can outlive the previous movie.  Blank
        // it just before the decoder starts, or its old final frame is shown
        // until FFmpeg posts the first frame of this file.  The surface is
        // visible here, so the lock cannot wait on a detached consumer.
        clearVideoSurface(surface);
        videoStarted = true;
        videoHandle = nativeStartVideo(path, surface, movieClock, startMs);
        if (videoHandle == 0) {
            videoStarted = false;
            videoStartFailed = true;
        }
    }

    /** Stop and join the decoder thread; called on the main thread only. */
    private void stopVideo() {
        long handle = videoHandle;
        videoHandle = 0;
        videoStarted = false;
        if (handle != 0) nativeStopVideo(handle);
    }

    /**
     * Replace the previous movie's last posted frame with opaque black.  The
     * SurfaceView and its buffer queue can outlive the player, so without this
     * the old final frame stays visible until FFmpeg posts the first frame of
     * the next movie.  Called on the main thread from {@link #maybeStartVideo}
     * after the surface has become visible.
     */
    private void clearVideoSurface(Surface surface) {
        if (surface == null || !surface.isValid()) return;
        try {
            nativeClearVideoSurface(surface);
        } catch (Throwable ignored) {
            // A missing placeholder must never abort the movie.
        }
    }

    private void applySurfaceFrameRate() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        Surface surface = surfaceView.getHolder().getSurface();
        if (surface == null || !surface.isValid()) return;
        try {
            if (movieFps > 0.0f) {
                surface.setFrameRate(movieFps,
                        Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            } else {
                surface.setFrameRate(0.0f,
                        Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            }
        } catch (Throwable ignored) {
            // The timestamped render path still works without the hint.
        }
    }

    private void clearSurfaceFrameRate() {
        movieFps = 0.0f;
        applySurfaceFrameRate();
    }

    /**
     * Release everything the finished movie owns: the clock ticker, the
     * joined FFmpeg decoder thread, its ANativeWindow and clock reference, the
     * diagnostics listener, and ExoPlayer with its audio thread, codecs and
     * audio focus.  Called on the main thread only, before the guest is told
     * that the movie ended.
     */
    private void releasePlayer() {
        handler.removeCallbacks(clockTick);
        stopVideo();
        movieClock.setPositionMs(0);
        moviePath = null;
        videoGeneration = 0;
        videoStartFailed = false;
        clearSurfaceFrameRate();

        Object trace = diagnostics;
        diagnostics = null;
        MovieDiagnosticsBridge.detach(trace);

        ExoPlayer exo = player;
        player = null;
        if (exo == null) return;

        try {
            exo.stop();
        } catch (Throwable ignored) {
        }
        try {
            exo.release();
        } catch (Throwable ignored) {
        }
    }
}
