package wc3.recomp;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.net.Uri;
import android.util.DisplayMetrics;
import android.view.Gravity;
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
 * host side of that facade.  Android has no AVI extractor, so playback is
 * handed to Media3 with the Media3Avi extractor: it owns RIFF/AVI sample
 * ordering, MPEG-4 Part 2 B-frame timestamps, MP3 audio, A/V synchronisation
 * and frame pacing.  Video still decodes through the platform MediaCodec and
 * is rendered directly to a SurfaceView; there is no per-frame Java copy and
 * no custom clock.
 *
 * <p>The movie layer is a separate SurfaceView above GameActivity's surface.
 * While the movie owns the screen, the D3D9 bridge skips guest presents, so
 * DXVK does not compete with the decoder for the GPU.
 */
@SuppressWarnings("UnstableApiUsage")
final class MoviePlayer {
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

    private ExoPlayer player;

    MoviePlayer(Activity activity, Host host) {
        this.activity = activity;
        this.host = host;

        FrameLayout frame = new FrameLayout(activity);
        frame.setBackgroundColor(Color.BLACK);

        surfaceView = new SurfaceView(activity);
        surfaceView.setZOrderOnTop(true);
        surfaceView.setFocusable(false);
        surfaceView.setFocusableInTouchMode(false);
        surfaceView.getHolder().setFormat(PixelFormat.OPAQUE);

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
                startPlayer(path, gen);
            } catch (Throwable ignored) {
                finish(gen, 1);
            }
        });
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

    private void finish(int gen, int status) {
        if (gen != generation.get()) return;
        if (!finished.compareAndSet(false, true)) return;
        generation.incrementAndGet();

        activity.runOnUiThread(() -> {
            releasePlayer();
            layer.setVisibility(View.GONE);
            host.setOverlayVisible(true);
        });
        host.onFinished(status);
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
    }

    private void startPlayer(String path, int gen) {
        if (gen != generation.get()) return;

        ExoPlayer exo = new ExoPlayer.Builder(activity)
                .setMediaSourceFactory(new DefaultMediaSourceFactory(
                        activity, new AviExtractorsFactory()))
                .build();
        player = exo;

        exo.setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(C.USAGE_MEDIA)
                .setContentType(C.AUDIO_CONTENT_TYPE_MOVIE)
                .build(), true);
        exo.setVideoSurfaceView(surfaceView);
        exo.addListener(new Player.Listener() {
            @Override
            public void onPlaybackStateChanged(int playbackState) {
                if (playbackState == Player.STATE_ENDED) {
                    finish(gen, 0);
                }
            }

            @Override
            public void onPlayerError(PlaybackException error) {
                finish(gen, 1);
            }
        });
        exo.setMediaItem(MediaItem.fromUri(
                Uri.fromFile(new File(path))));
        exo.prepare();
        exo.play();
    }

    private void releasePlayer() {
        ExoPlayer exo = player;
        player = null;
        if (exo == null) return;

        try {
            exo.stop();
        } catch (Throwable ignored) {
        }
        try {
            exo.clearVideoSurface();
        } catch (Throwable ignored) {
        }
        try {
            exo.release();
        } catch (Throwable ignored) {
        }
    }
}
