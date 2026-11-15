package wc3.recomp;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.media.AudioAttributes;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.media.MediaPlayer;
import android.os.Build;
import android.os.Process;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Campaign movie player.
 *
 * <p>The game talks to a DirectShow facade in native code.  This class is the
 * host side of that facade: it parses the AVI (Android has no AVI extractor),
 * decodes the MPEG-4 Part 2 video with MediaCodec directly to a SurfaceView,
 * and plays the extracted MP3 track with MediaPlayer.  Audio and video are
 * clocked from the same start instant; MediaCodec supplies presentation
 * timestamps in display order, which already accounts for B-frames.
 *
 * <p>Decoded frames are handed to SurfaceFlinger with an explicit render
 * timestamp instead of sleeping in Java and flipping immediately.  While the
 * movie owns the screen, the D3D9 bridge skips guest presents, so DXVK does
 * not compete with the decoder for the GPU.
 *
 * <p>The movie layer is a separate SurfaceView above GameActivity's surface.
 * There is no per-frame bridge and no copy into a Java Bitmap.
 */
final class MoviePlayer {
    interface Host {
        void setOverlayVisible(boolean visible);

        void onFinished(int status);
    }

    private static final String VIDEO_MIME = "video/mp4v-es";
    private static final long SURFACE_WAIT_NS = 10_000_000_000L;
    private static final long MAX_IDLE_AFTER_EOS_NS = 15_000_000_000L;
    private static final long EXTRA_RUN_NS = 30_000_000_000L;

    private final Activity activity;
    private final Host host;
    private final FrameLayout layer;
    private final SurfaceView surfaceView;
    private final Object surfaceLock = new Object();
    private final Object audioLock = new Object();
    private final AtomicInteger generation = new AtomicInteger();
    private final AtomicBoolean finished = new AtomicBoolean();

    private boolean surfaceReady;
    private volatile boolean surfaceLost;
    private MediaPlayer audio;

    private final SurfaceHolder.Callback surfaceCallback =
            new SurfaceHolder.Callback() {
                @Override
                public void surfaceCreated(SurfaceHolder holder) {
                    synchronized (surfaceLock) {
                        surfaceReady = true;
                        surfaceLost = false;
                        surfaceLock.notifyAll();
                    }
                }

                @Override
                public void surfaceChanged(SurfaceHolder holder, int format,
                                           int width, int height) {
                }

                @Override
                public void surfaceDestroyed(SurfaceHolder holder) {
                    synchronized (surfaceLock) {
                        surfaceReady = false;
                        surfaceLost = true;
                        surfaceLock.notifyAll();
                    }
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
        surfaceView.getHolder().setFormat(PixelFormat.OPAQUE);
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
        surfaceLost = false;

        activity.runOnUiThread(() -> {
            if (gen != generation.get()) return;
            releaseAudio();
        });

        Thread worker = new Thread(() -> run(path, gen), "wc3-movie");
        worker.setDaemon(true);
        worker.start();
    }

    /** Called when the guest stops the graph or the activity is destroyed. */
    void stop() {
        generation.incrementAndGet();
        wakeSurface();
        activity.runOnUiThread(() -> {
            clearSurfaceFrameRate();
            layer.setVisibility(View.GONE);
            host.setOverlayVisible(true);
            releaseAudio();
        });
    }

    private void finish(int gen, int status) {
        if (gen != generation.get()) return;
        if (!finished.compareAndSet(false, true)) return;
        generation.incrementAndGet();
        wakeSurface();

        activity.runOnUiThread(() -> {
            clearSurfaceFrameRate();
            layer.setVisibility(View.GONE);
            host.setOverlayVisible(true);
            releaseAudio();
        });
        host.onFinished(status);
    }

    /**
     * Size the SurfaceView to the movie's aspect ratio inside the full-screen
     * layer, then make the layer visible and wait for its surface.  The layer
     * must stay GONE until the final LayoutParams are set: resizing a
     * SurfaceView that already owns a surface destroys that surface, and a
     * decoder configured with the old Surface then sees surfaceLost and
     * reports an immediate failure.  Parsing first also means the layer is
     * shown at its letterboxed size from the first frame.
     */
    private boolean showSurface(int videoWidth, int videoHeight, int gen) {
        if (videoWidth <= 0 || videoHeight <= 0) return false;
        CountDownLatch changed = new CountDownLatch(1);
        activity.runOnUiThread(() -> {
            try {
                if (gen != generation.get()) return;
                // The movie layer is still GONE here, so its own width and
                // height are zero.  Use its parent (the activity content
                // view), which has the same bounds the game surface uses;
                // DisplayMetrics includes system-bar area that the game
                // surface does not, and using it would clip the movie on
                // devices with a visible navigation bar.
                DisplayMetrics dm = activity.getResources().getDisplayMetrics();
                View container = layer.getParent() instanceof View
                        ? (View) layer.getParent() : null;
                int containerWidth = container != null
                        && container.getWidth() > 0
                        ? container.getWidth() : dm.widthPixels;
                int containerHeight = container != null
                        && container.getHeight() > 0
                        ? container.getHeight() : dm.heightPixels;
                if (containerWidth <= 0 || containerHeight <= 0) return;
                double scale = Math.min(containerWidth / (double) videoWidth,
                        containerHeight / (double) videoHeight);
                int width = Math.max(1, (int) Math.round(videoWidth * scale));
                int height = Math.max(1, (int) Math.round(videoHeight * scale));
                synchronized (surfaceLock) {
                    surfaceReady = false;
                    surfaceLost = false;
                }
                surfaceView.setLayoutParams(new FrameLayout.LayoutParams(
                        width, height, Gravity.CENTER));
                host.setOverlayVisible(false);
                layer.setVisibility(View.VISIBLE);
            } finally {
                changed.countDown();
            }
        });

        try {
            if (!changed.await(2L, TimeUnit.SECONDS)) return false;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        return awaitSurface(gen);
    }

    private boolean current(int gen) {
        return gen == generation.get() && !Thread.currentThread().isInterrupted();
    }

    private void wakeSurface() {
        synchronized (surfaceLock) {
            surfaceLock.notifyAll();
        }
    }

    private void run(String path, int gen) {
        // Priority is a hint only; losing it must not skip finish() and leave
        // the guest blocked in its movie pump forever.
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_DISPLAY);
        } catch (Throwable ignored) {
        }

        RandomAccessFile raf = null;
        MediaCodec codec = null;
        try {
            File movie = new File(path);
            AviFile avi = AviFile.parse(movie);
            File audioFile = avi.audio.isEmpty()
                    ? null : extractAudio(movie, avi);

            if (!showSurface(avi.width, avi.height, gen)) {
                finish(gen, 1);
                return;
            }
            if (!current(gen)) return;

            Surface surface = surfaceView.getHolder().getSurface();
            if (surface == null || !surface.isValid()) {
                finish(gen, 1);
                return;
            }
            setSurfaceFrameRate(surface, avi.fpsNum / (float) avi.fpsDen);

            MediaFormat format = MediaFormat.createVideoFormat(
                    VIDEO_MIME, avi.width, avi.height);
            format.setByteBuffer("csd-0", ByteBuffer.wrap(avi.csd));
            format.setInteger(MediaFormat.KEY_FRAME_RATE,
                    Math.max(1, Math.round(avi.fpsNum / (float) avi.fpsDen)));
            if (avi.maxVideoSize > 0) {
                format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE,
                        avi.maxVideoSize);
            }

            codec = MediaCodec.createDecoderByType(VIDEO_MIME);
            codec.configure(format, surface, null, 0);
            codec.start();

            raf = new RandomAccessFile(movie, "r");
            boolean ok = decode(raf, avi, codec, gen, audioFile);
            finish(gen, ok ? 0 : 1);
        } catch (Throwable ignored) {
            finish(gen, 1);
        } finally {
            if (codec != null) {
                try {
                    codec.stop();
                } catch (Throwable ignored) {
                }
                try {
                    codec.release();
                } catch (Throwable ignored) {
                }
            }
            if (raf != null) {
                try {
                    raf.close();
                } catch (Throwable ignored) {
                }
            }
        }
    }

    /**
     * Tell SurfaceFlinger the content cadence.  On displays that can switch
     * modes this lets it pick 24/48/120 Hz instead of repeating a 24 fps
     * stream on a 60 Hz panel with a 3:2 cadence.  A non-positive rate clears
     * the hint when the movie surface goes away, so the game does not inherit
     * a 24 Hz display mode.
     */
    private static void setSurfaceFrameRate(Surface surface, float fps) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || surface == null)
            return;
        try {
            if (!surface.isValid()) return;
            if (fps > 0.0f) {
                surface.setFrameRate(fps,
                        Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            } else {
                surface.setFrameRate(0.0f,
                        Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            }
        } catch (Throwable ignored) {
            // A surface that predates the hint or a device without the
            // extension still renders; the timestamp scheduling below is the
            // actual pacing mechanism.
        }
    }

    private void clearSurfaceFrameRate() {
        setSurfaceFrameRate(surfaceView.getHolder().getSurface(), 0.0f);
    }

    private boolean decode(RandomAccessFile raf, AviFile avi,
                           MediaCodec codec, int gen, File audioFile)
            throws IOException, InterruptedException {
        int count = avi.video.size();
        if (count == 0) return false;

        byte[] input = new byte[Math.max(1, avi.maxVideoSize)];
        MediaCodec.BufferInfo outputInfo = new MediaCodec.BufferInfo();
        int queued = 0;
        int rendered = 0;
        int outputFrames = 0;
        boolean eosQueued = false;
        boolean outputEos = false;
        boolean clockStarted = false;
        long frameIntervalNs =
                1_000_000_000L * avi.fpsDen / avi.fpsNum;
        if (frameIntervalNs <= 0L) return false;

        // Keep at most two frames in the compositor queue, and use the codec
        // output timestamp as the presentation time instead of sleeping and
        // flipping immediately.  This is the same scheduling path MediaPlayer
        // uses: SurfaceFlinger latches the buffer on the right vsync.
        long releaseLeadNs = frameIntervalNs * 2L;
        long lateDropNs = frameIntervalNs * 2L;
        long movieNs = (long) count * frameIntervalNs;
        long startNs = 0L;
        long startupNs = 0L;
        long deadlineNs = 0L;
        long startupDeadlineNs = System.nanoTime() + 15_000_000_000L;
        long lastOutputNs = System.nanoTime();

        while (current(gen) && !surfaceLost) {
            for (int i = 0; i < 8 && !eosQueued; i++) {
                int inIndex = codec.dequeueInputBuffer(0);
                if (inIndex < 0) break;

                if (queued >= count) {
                    codec.queueInputBuffer(inIndex, 0, 0, 0,
                            MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                    eosQueued = true;
                    break;
                }

                AviFile.Sample sample = avi.video.get(queued);
                if (sample.size > input.length) return false;

                raf.seek(sample.offset);
                raf.readFully(input, 0, sample.size);
                ByteBuffer buffer = codec.getInputBuffer(inIndex);
                if (buffer == null || sample.size > buffer.capacity()) {
                    return false;
                }
                buffer.clear();
                buffer.put(input, 0, sample.size);

                long ptsUs = avi.presentationTimeUs(sample.display);
                int flags = sample.type == AviFile.TYPE_I
                        ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0;
                codec.queueInputBuffer(inIndex, 0, sample.size, ptsUs, flags);
                queued++;
            }

            int outIndex = codec.dequeueOutputBuffer(outputInfo, 10_000);
            if (outIndex == MediaCodec.INFO_TRY_AGAIN_LATER) {
                long now = System.nanoTime();
                if (!clockStarted) {
                    if (now > startupDeadlineNs) return false;
                    continue;
                }
                if (eosQueued
                        && now - lastOutputNs > MAX_IDLE_AFTER_EOS_NS) {
                    return rendered > 0;
                }
                if (now > deadlineNs) return false;
                continue;
            }
            if (outIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) continue;
            if (outIndex < 0) continue;

            lastOutputNs = System.nanoTime();
            if ((outputInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                outputEos = true;
            }

            // With a Surface output, BufferInfo.size is not a frame
            // indication: many decoders report 0 for every decoded frame.
            // The old size > 0 test therefore skipped most frames and made
            // the movie stutter.  A zero-sized buffer is only an empty
            // end-of-stream marker (or codec config) to be discarded.
            boolean codecConfig =
                    (outputInfo.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
            boolean emptyEos = outputEos && outputInfo.size == 0
                    && outputInfo.presentationTimeUs == 0L;
            if (codecConfig || emptyEos) {
                codec.releaseOutputBuffer(outIndex, false);
            } else {
                if (!clockStarted) {
                    // Start the audio clock only when the decoder has a real
                    // frame to show.  Starting MediaPlayer earlier made the
                    // audio lead the video by the whole decoder warm-up.
                    long audioNs = audioFile == null
                            ? 0L : startAudio(audioFile, gen);
                    startNs = audioNs != 0L ? audioNs : System.nanoTime();
                    startupNs = startNs + 500_000_000L;
                    deadlineNs = startNs + movieNs + EXTRA_RUN_NS;
                    lastOutputNs = System.nanoTime();
                    clockStarted = true;
                    if (!current(gen)) {
                        codec.releaseOutputBuffer(outIndex, false);
                        break;
                    }
                }
                // The codec is expected to return the input presentation
                // times in display order.  A decoder that loses them and
                // reports zero would otherwise make every frame look like it
                // belongs at t=0; the container is constant-rate, so the
                // output index is an exact fallback timeline.
                long ptsUs = outputInfo.presentationTimeUs;
                if (ptsUs == 0L && outputFrames > 0) {
                    ptsUs = avi.presentationTimeUs(outputFrames);
                }
                outputFrames++;
                long targetNs = startNs + ptsUs * 1_000L;
                long now = System.nanoTime();
                if (now > startupNs && now > targetNs + lateDropNs) {
                    // The frame already missed its slot.  Showing it now would
                    // push every following frame further behind; drop it and
                    // keep the audio clock authoritative.
                    codec.releaseOutputBuffer(outIndex, false);
                } else {
                    waitUntil(targetNs - releaseLeadNs, gen);
                    if (!current(gen) || surfaceLost) {
                        codec.releaseOutputBuffer(outIndex, false);
                        break;
                    }
                    long renderNs = Math.max(
                            System.nanoTime() + 1_000_000L, targetNs);
                    codec.releaseOutputBuffer(outIndex, renderNs);
                    rendered++;
                }
            }

            if (outputEos) break;
        }

        return rendered > 0 && (outputEos || eosQueued)
                && current(gen) && !surfaceLost;
    }

    private void waitUntil(long targetNs, int gen) throws InterruptedException {
        while (current(gen) && !surfaceLost) {
            long now = System.nanoTime();
            if (now >= targetNs) return;
            long sleepMs = Math.min(10L,
                    Math.max(1L, (targetNs - now) / 1_000_000L));
            Thread.sleep(sleepMs);
        }
    }

    private boolean awaitSurface(int gen) {
        long deadline = System.nanoTime() + SURFACE_WAIT_NS;
        synchronized (surfaceLock) {
            // A destroy callback can race the first create: keep waiting for
            // the replacement instead of treating the transient loss as the
            // final failure.
            while ((!surfaceReady || surfaceLost) && current(gen)) {
                long remaining = deadline - System.nanoTime();
                if (remaining <= 0L) break;
                try {
                    surfaceLock.wait(Math.max(1L,
                            remaining / 1_000_000L));
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
            return surfaceReady && current(gen) && !surfaceLost;
        }
    }

    /**
     * Starts the native audio path and returns the System.nanoTime() at which
     * it actually began.  Video presentation is scheduled against this clock,
     * not against the instant the Java decode loop happens to start, so the
     * two streams share one time base even when MediaPlayer preparation takes
     * longer than expected.
     */
    private long startAudio(final File file, final int gen) {
        final CountDownLatch latch = new CountDownLatch(1);
        final AtomicLong startedNs = new AtomicLong();

        activity.runOnUiThread(() -> {
            MediaPlayer player = null;
            try {
                player = new MediaPlayer();
                player.setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                        .build());
                player.setDataSource(file.getAbsolutePath());
                player.prepare();
                if (gen != generation.get()) {
                    player.release();
                    return;
                }
                player.start();
                startedNs.set(System.nanoTime());
                synchronized (audioLock) {
                    audio = player;
                    player = null;
                }
            } catch (Throwable ignored) {
                if (player != null) {
                    try {
                        player.release();
                    } catch (Throwable ignoredAgain) {
                    }
                }
            } finally {
                latch.countDown();
            }
        });

        try {
            latch.await(10L, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return startedNs.get();
    }

    private void releaseAudio() {
        MediaPlayer player;
        synchronized (audioLock) {
            player = audio;
            audio = null;
        }
        if (player == null) return;
        try {
            player.stop();
        } catch (Throwable ignored) {
        }
        try {
            player.release();
        } catch (Throwable ignored) {
        }
    }

    private File extractAudio(File movie, AviFile avi) throws IOException {
        File directory = new File(activity.getCacheDir(), "movies");
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("movie cache");
        }

        String key = Integer.toHexString(movie.getAbsolutePath().hashCode())
                + "-" + movie.length() + "-" + movie.lastModified() + ".mp3";
        File output = new File(directory, key);
        if (output.isFile() && output.length() > 0L) return output;

        File temporary = new File(directory, key + ".tmp");
        try (FileOutputStream stream = new FileOutputStream(temporary);
             RandomAccessFile raf = new RandomAccessFile(movie, "r")) {
            byte[] buffer = new byte[64 * 1024];
            for (AviFile.Sample sample : avi.audio) {
                raf.seek(sample.offset);
                int left = sample.size;
                while (left > 0) {
                    int take = Math.min(left, buffer.length);
                    int read = raf.read(buffer, 0, take);
                    if (read <= 0) throw new IOException("truncated audio");
                    stream.write(buffer, 0, read);
                    left -= read;
                }
            }
            stream.getFD().sync();
        } catch (IOException e) {
            temporary.delete();
            throw e;
        }

        if (!temporary.renameTo(output)) {
            temporary.delete();
            throw new IOException("movie cache rename");
        }
        return output;
    }
}
