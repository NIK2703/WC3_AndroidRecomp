/*
 * movie_ffmpeg.c - campaign movie video decoder.
 *
 * The platform MPEG-4 decoder on the target device is Simple Profile only.
 * Campaign movies are MPEG-4 Part 2 Advanced Simple Profile with B-frames, so
 * MediaCodec can accept the format but cannot be trusted to reorder frames.
 * This module decodes the video stream with FFmpeg and presents display-order
 * frames directly to the movie SurfaceView through ANativeWindow.  Media3
 * keeps the AVI audio path and supplies the playback clock; the Java side
 * updates MovieClock from ExoPlayer and the video thread paces frames against
 * that clock.
 */
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>

#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_MOVIE
#define MOVIE_FFLOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "wc3movie", __VA_ARGS__)
#define MOVIE_FFLOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, "wc3movie", __VA_ARGS__)
#else
#define MOVIE_FFLOG(...) ((void)0)
#define MOVIE_FFLOGE(...) ((void)0)
#endif

#define RENDER_LEAD_MS 5
#define LATE_DROP_MS 100
#define START_SKIP_TOLERANCE_US 1000

/* FFmpeg's default callback writes to stderr, which Android discards for an
 * app process.  Debug builds forward errors and warnings to the same
 * wc3movie logcat tag as the rest of the player; release builds keep FFmpeg
 * quiet instead of formatting text nobody can read. */
#if WC3_DEBUG && WC3_DEBUG_MOVIE
static void movie_av_log(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl;
    int priority = level <= AV_LOG_ERROR ? ANDROID_LOG_ERROR
                                         : ANDROID_LOG_WARN;
    __android_log_vprint(priority, "wc3movie", fmt, vl);
}
#endif

static void movie_log_setup(void)
{
#if WC3_DEBUG && WC3_DEBUG_MOVIE
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(movie_av_log);
#else
    av_log_set_level(AV_LOG_QUIET);
#endif
}

static pthread_once_t movie_log_once = PTHREAD_ONCE_INIT;

typedef struct {
    JavaVM *vm;
    jobject clock;
    jmethodID clock_position_ms;
    ANativeWindow *window;
    char *path;
    int64_t start_us;

    _Atomic int stop;
    pthread_t thread;
    int thread_started;

#if WC3_DEBUG && WC3_DEBUG_MOVIE
    int64_t frames_decoded;
    int64_t frames_rendered;
    int64_t frames_dropped;
    int64_t frames_skipped;
    int64_t nonmonotonic;
    int64_t decode_us;
    int64_t render_us;
    int64_t max_late_ms;
#endif
} MovieVideo;

#if WC3_DEBUG && WC3_DEBUG_MOVIE
static int64_t monotonic_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

static void sleep_ms(int ms)
{
    if (ms <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static void log_av_error(const char *what, int error)
{
#if WC3_DEBUG && WC3_DEBUG_MOVIE
    char text[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, text, sizeof(text));
    MOVIE_FFLOGE("[ffmpeg] %s: %s (%d)", what, text, error);
#else
    (void)what;
    (void)error;
#endif
}

static int64_t clock_position_ms(JNIEnv *env, MovieVideo *movie)
{
    if (!movie->clock || !movie->clock_position_ms)
        return 0;
    jlong value = (*env)->CallLongMethod(env, movie->clock,
                                         movie->clock_position_ms);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    return value > 0 ? (int64_t)value : 0;
}

/* The container supplies display-order PTS, including for the B-frames that
 * are stored out of order.  Best-effort timestamp carries that value forward;
 * packet order is decode order and must not drive pacing. */
static int64_t frame_pts_us(const AVFrame *frame, const AVRational *time_base,
                            int64_t fallback_us)
{
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE)
        pts = frame->pts;
    if (pts == AV_NOPTS_VALUE)
        return fallback_us;
    return av_rescale_q(pts, *time_base, AV_TIME_BASE_Q);
}

static int render_frame(JNIEnv *env, MovieVideo *movie, AVFrame *frame,
                        struct SwsContext **sws, int *src_width, int *src_height,
                        int *src_format, int64_t pts_us)
{
    int64_t target_ms = pts_us / 1000;
    if (target_ms < 0)
        target_ms = 0;

    int64_t position_ms = clock_position_ms(env, movie);
    while (!atomic_load_explicit(&movie->stop, memory_order_relaxed) &&
           position_ms + RENDER_LEAD_MS < target_ms) {
        int64_t remaining = target_ms - position_ms - RENDER_LEAD_MS;
        sleep_ms(remaining > 5 ? 5 : (int)remaining);
        position_ms = clock_position_ms(env, movie);
    }

    if (atomic_load_explicit(&movie->stop, memory_order_relaxed))
        return 1;

    int64_t late_ms = position_ms - target_ms;
#if WC3_DEBUG && WC3_DEBUG_MOVIE
    if (late_ms > movie->max_late_ms)
        movie->max_late_ms = late_ms;
#endif
    if (late_ms > LATE_DROP_MS) {
#if WC3_DEBUG && WC3_DEBUG_MOVIE
        movie->frames_dropped++;
#endif
        return 0;
    }

    if (!*sws || *src_width != frame->width || *src_height != frame->height ||
        *src_format != frame->format) {
        if (*sws)
            sws_freeContext(*sws);
        *sws = sws_getContext(frame->width, frame->height,
                              (enum AVPixelFormat)frame->format,
                              frame->width, frame->height, AV_PIX_FMT_RGBA,
                              SWS_BILINEAR, NULL, NULL, NULL);
        if (!*sws) {
            MOVIE_FFLOGE("[ffmpeg] sws_getContext failed %dx%d fmt=%d",
                         frame->width, frame->height, frame->format);
            return -1;
        }
        *src_width = frame->width;
        *src_height = frame->height;
        *src_format = frame->format;

        int rc = ANativeWindow_setBuffersGeometry(
                movie->window, frame->width, frame->height,
                WINDOW_FORMAT_RGBA_8888);
        if (rc != 0)
            MOVIE_FFLOGE("[ffmpeg] setBuffersGeometry rc=%d", rc);
    }

#if WC3_DEBUG && WC3_DEBUG_MOVIE
    int64_t render_started = monotonic_us();
#endif
    ANativeWindow_Buffer buffer;
    int rc = ANativeWindow_lock(movie->window, &buffer, NULL);
    if (rc != 0) {
        MOVIE_FFLOGE("[ffmpeg] ANativeWindow_lock rc=%d", rc);
        return -1;
    }
    if (!buffer.bits || buffer.stride <= 0 ||
        buffer.width < frame->width || buffer.height < frame->height) {
        MOVIE_FFLOGE("[ffmpeg] bad window buffer %dx%d stride=%d bits=%p"
                     " (frame %dx%d)",
                     buffer.width, buffer.height, buffer.stride, buffer.bits,
                     frame->width, frame->height);
        (void)ANativeWindow_unlockAndPost(movie->window);
        return -1;
    }
    if (buffer.format != WINDOW_FORMAT_RGBA_8888 &&
        buffer.format != WINDOW_FORMAT_RGBX_8888) {
        MOVIE_FFLOGE("[ffmpeg] unexpected window format %d", buffer.format);
        (void)ANativeWindow_unlockAndPost(movie->window);
        return -1;
    }

#if WC3_DEBUG && WC3_DEBUG_MOVIE
    if (movie->frames_rendered == 0) {
        MOVIE_FFLOG("[ffmpeg] window buffer %dx%d stride=%d fmt=%d frame=%dx%d",
                    buffer.width, buffer.height, buffer.stride, buffer.format,
                    frame->width, frame->height);
    }
#endif
    uint8_t *dst_data[4] = { (uint8_t *)buffer.bits, NULL, NULL, NULL };
    int dst_linesize[4] = { buffer.stride * 4, 0, 0, 0 };
    sws_scale(*sws, (const uint8_t *const *)frame->data,
              frame->linesize, 0, frame->height, dst_data, dst_linesize);
    rc = ANativeWindow_unlockAndPost(movie->window);
#if WC3_DEBUG && WC3_DEBUG_MOVIE
    movie->render_us += monotonic_us() - render_started;
#endif
    if (rc != 0) {
        MOVIE_FFLOGE("[ffmpeg] ANativeWindow_unlockAndPost rc=%d", rc);
        return -1;
    }
#if WC3_DEBUG && WC3_DEBUG_MOVIE
    movie->frames_rendered++;
#endif
    return 0;
}

static int drain_decoder(JNIEnv *env, MovieVideo *movie, AVCodecContext *ctx,
                         AVFrame *frame, struct SwsContext **sws, int *src_width,
                         int *src_height, int *src_format,
                         const AVRational *time_base, int64_t frame_duration_us,
                         int64_t *last_pts_us)
{
    for (;;) {
#if WC3_DEBUG && WC3_DEBUG_MOVIE
        int64_t receive_started = monotonic_us();
#endif
        int ret = avcodec_receive_frame(ctx, frame);
#if WC3_DEBUG && WC3_DEBUG_MOVIE
        if (ret >= 0)
            movie->decode_us += monotonic_us() - receive_started;
#endif
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            log_av_error("avcodec_receive_frame", ret);
            return -1;
        }

        /* MPEG-4 codes 1088 lines and crops the display to 872.  FFmpeg
         * normally applies the decoder's crop rectangle before returning the
         * frame; keep the explicit call for streams where the rectangle is
         * still attached, or the coded height would be scaled into the
         * 872-line view. */
        if (frame->crop_top || frame->crop_bottom ||
            frame->crop_left || frame->crop_right) {
            int crop_rc = av_frame_apply_cropping(frame, 0);
            if (crop_rc < 0)
                log_av_error("av_frame_apply_cropping", crop_rc);
        }

        int64_t fallback = *last_pts_us == INT64_MIN
                ? 0 : *last_pts_us + frame_duration_us;
        int64_t pts_us = frame_pts_us(frame, time_base, fallback);
#if WC3_DEBUG && WC3_DEBUG_MOVIE
        if (*last_pts_us != INT64_MIN && pts_us + 1000 < *last_pts_us)
            movie->nonmonotonic++;
        movie->frames_decoded++;
        if (movie->frames_decoded == 1) {
            MOVIE_FFLOG("[ffmpeg] first frame %dx%d fmt=%d pts=%" PRId64
                        "us crop=%d/%d/%d/%d cs=%d range=%d",
                        frame->width, frame->height, frame->format, pts_us,
                        (int)frame->crop_left, (int)frame->crop_top,
                        (int)frame->crop_right, (int)frame->crop_bottom,
                        frame->colorspace, frame->color_range);
        }
#endif
        *last_pts_us = pts_us;

        if (movie->start_us > 0 &&
            pts_us + START_SKIP_TOLERANCE_US < movie->start_us) {
#if WC3_DEBUG && WC3_DEBUG_MOVIE
            movie->frames_skipped++;
#endif
            av_frame_unref(frame);
            if (atomic_load_explicit(&movie->stop, memory_order_relaxed))
                return 1;
            continue;
        }

        int rc = render_frame(env, movie, frame, sws, src_width, src_height,
                              src_format, pts_us);
        av_frame_unref(frame);
        if (rc != 0)
            return rc;
        if (atomic_load_explicit(&movie->stop, memory_order_relaxed))
            return 1;
    }
}

static void *video_thread(void *arg)
{
    MovieVideo *movie = (MovieVideo *)arg;
    JNIEnv *env = NULL;
    AVFormatContext *format = NULL;
    AVCodecContext *codec = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    struct SwsContext *sws = NULL;
    int src_width = 0;
    int src_height = 0;
    int src_format = -1;
    int video_stream = -1;
    int saw_eof = 0;
    int64_t last_pts_us = INT64_MIN;
    int64_t frame_duration_us = 41667;

    (void)pthread_setname_np(pthread_self(), "wc3-movie-vid");
    if ((*movie->vm)->AttachCurrentThread(movie->vm, &env, NULL) != JNI_OK) {
        MOVIE_FFLOGE("[ffmpeg] AttachCurrentThread failed");
        return NULL;
    }

    int ret = avformat_open_input(&format, movie->path, NULL, NULL);
    if (ret < 0) {
        log_av_error("avformat_open_input", ret);
        goto done;
    }
    ret = avformat_find_stream_info(format, NULL);
    if (ret < 0) {
        log_av_error("avformat_find_stream_info", ret);
        goto done;
    }

    const AVCodec *decoder = NULL;
    video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1,
                                       &decoder, 0);
    if (video_stream < 0 || !decoder) {
        MOVIE_FFLOGE("[ffmpeg] no video stream (%d)", video_stream);
        goto done;
    }

    AVStream *stream = format->streams[video_stream];
    codec = avcodec_alloc_context3(decoder);
    if (!codec) {
        MOVIE_FFLOGE("[ffmpeg] avcodec_alloc_context3 failed");
        goto done;
    }
    ret = avcodec_parameters_to_context(codec, stream->codecpar);
    if (ret < 0) {
        log_av_error("avcodec_parameters_to_context", ret);
        goto done;
    }
    codec->pkt_timebase = stream->time_base;
    /* Frame threading delays the first output by thread_count frames, which
     * shows up as a startup burst of late frames.  The movies are 1080p ASP
     * at 24 fps; one software thread is enough to start in sync, and the
     * debug decode-time counter proves whether more threads are needed. */
    codec->thread_count = 1;
    ret = avcodec_open2(codec, decoder, NULL);
    if (ret < 0) {
        log_av_error("avcodec_open2", ret);
        goto done;
    }

    double fps = av_q2d(stream->avg_frame_rate);
    if (fps > 0.1 && fps < 240.0)
        frame_duration_us = (int64_t)(1000000.0 / fps + 0.5);
    MOVIE_FFLOG("[ffmpeg] start api=%s path=%s codec=%s %dx%d fps=%.3f"
                " tb=%d/%d start=%" PRId64 "ms threads=%d",
                av_version_info(), movie->path, decoder->name,
                codec->width, codec->height, fps,
                stream->time_base.num, stream->time_base.den,
                movie->start_us / 1000, codec->thread_count);

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        MOVIE_FFLOGE("[ffmpeg] packet/frame allocation failed");
        goto done;
    }

    if (movie->start_us > 0) {
        int64_t timestamp = av_rescale_q(movie->start_us, AV_TIME_BASE_Q,
                                         stream->time_base);
        ret = av_seek_frame(format, video_stream, timestamp,
                            AVSEEK_FLAG_BACKWARD);
        if (ret < 0)
            log_av_error("av_seek_frame", ret);
    }

#if WC3_DEBUG && WC3_DEBUG_MOVIE
    int64_t last_report_us = monotonic_us();
#endif
    for (;;) {
        if (atomic_load_explicit(&movie->stop, memory_order_relaxed))
            break;

        ret = av_read_frame(format, packet);
        if (ret == AVERROR_EOF) {
            saw_eof = 1;
            break;
        }
        if (ret < 0) {
            log_av_error("av_read_frame", ret);
            break;
        }
        if (packet->stream_index != video_stream) {
            av_packet_unref(packet);
            continue;
        }

        int drains = 0;
        for (;;) {
#if WC3_DEBUG && WC3_DEBUG_MOVIE
            int64_t send_started = monotonic_us();
#endif
            ret = avcodec_send_packet(codec, packet);
#if WC3_DEBUG && WC3_DEBUG_MOVIE
            movie->decode_us += monotonic_us() - send_started;
#endif
            if (ret != AVERROR(EAGAIN))
                break;
            /* A healthy decoder frees at least one frame per EAGAIN.  The
             * bound turns an inconsistent decoder into a stopped movie
             * instead of an endless loop on the video thread. */
            if (++drains > 32) {
                MOVIE_FFLOGE("[ffmpeg] send_packet EAGAIN without progress");
                av_packet_unref(packet);
                goto done;
            }
            ret = drain_decoder(env, movie, codec, frame, &sws, &src_width,
                                &src_height, &src_format, &stream->time_base,
                                frame_duration_us, &last_pts_us);
            if (ret != 0) {
                av_packet_unref(packet);
                goto done;
            }
        }
        av_packet_unref(packet);
        if (ret < 0) {
            if (ret != AVERROR_EOF)
                log_av_error("avcodec_send_packet", ret);
            break;
        }

        ret = drain_decoder(env, movie, codec, frame, &sws, &src_width,
                            &src_height, &src_format, &stream->time_base,
                            frame_duration_us, &last_pts_us);
        if (ret != 0)
            break;

#if WC3_DEBUG && WC3_DEBUG_MOVIE
        int64_t now = monotonic_us();
        if (now - last_report_us >= 1000000) {
            MOVIE_FFLOG("[ffmpeg] pos=%" PRId64 "ms decoded=%" PRId64
                        " rendered=%" PRId64 " dropped=%" PRId64
                        " skipped=%" PRId64 " nonmono=%" PRId64
                        " decode=%.1fms/frame render=%.1fms/frame lateMax=%"
                        PRId64 "ms",
                        clock_position_ms(env, movie), movie->frames_decoded,
                        movie->frames_rendered, movie->frames_dropped,
                        movie->frames_skipped, movie->nonmonotonic,
                        movie->frames_decoded
                                ? (double)movie->decode_us / 1000.0 /
                                          movie->frames_decoded
                                : 0.0,
                        movie->frames_rendered
                                ? (double)movie->render_us / 1000.0 /
                                          movie->frames_rendered
                                : 0.0,
                        movie->max_late_ms);
            last_report_us = now;
        }
#endif
    }

    if (saw_eof &&
        atomic_load_explicit(&movie->stop, memory_order_relaxed) == 0) {
        ret = avcodec_send_packet(codec, NULL);
        if (ret == AVERROR(EAGAIN)) {
            (void)drain_decoder(env, movie, codec, frame, &sws, &src_width,
                                &src_height, &src_format, &stream->time_base,
                                frame_duration_us, &last_pts_us);
            ret = avcodec_send_packet(codec, NULL);
        }
        if (ret < 0 && ret != AVERROR_EOF)
            log_av_error("avcodec_send_packet flush", ret);
        (void)drain_decoder(env, movie, codec, frame, &sws, &src_width,
                            &src_height, &src_format, &stream->time_base,
                            frame_duration_us, &last_pts_us);
    }

done:
#if WC3_DEBUG && WC3_DEBUG_MOVIE
    MOVIE_FFLOG("[ffmpeg] stop eof=%d stopped=%d decoded=%" PRId64
                " rendered=%" PRId64 " dropped=%" PRId64
                " skipped=%" PRId64 " nonmono=%" PRId64
                " lateMax=%" PRId64 "ms",
                saw_eof,
                (int)atomic_load_explicit(&movie->stop, memory_order_relaxed),
                movie->frames_decoded, movie->frames_rendered,
                movie->frames_dropped, movie->frames_skipped,
                movie->nonmonotonic, movie->max_late_ms);
#endif
    if (sws)
        sws_freeContext(sws);
    if (frame)
        av_frame_free(&frame);
    if (packet)
        av_packet_free(&packet);
    if (codec)
        avcodec_free_context(&codec);
    if (format)
        avformat_close_input(&format);
    if (env)
        (*movie->vm)->DetachCurrentThread(movie->vm);
    return NULL;
}

/* Blank the SurfaceView before a new movie becomes visible.  The SurfaceView
 * and its buffer queue can outlive the previous player; without this clear the
 * last frame of that movie stays on screen until FFmpeg posts the first frame
 * of the new file.  Only the two 32-bit RGB formats used by MoviePlayer are
 * handled here. */
JNIEXPORT void JNICALL
Java_wc3_recomp_MoviePlayer_nativeClearVideoSurface(
        JNIEnv *env, jclass thiz, jobject surface)
{
    (void)thiz;
    if (!env || !surface)
        return;

    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window)
        return;

    int format = ANativeWindow_getFormat(window);
    if (format != WINDOW_FORMAT_RGBA_8888 &&
        format != WINDOW_FORMAT_RGBX_8888) {
        MOVIE_FFLOG("[ffmpeg] clear surface: format=%d", format);
        ANativeWindow_release(window);
        return;
    }

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, NULL) != 0) {
        MOVIE_FFLOGE("[ffmpeg] clear surface: lock failed");
        ANativeWindow_release(window);
        return;
    }
    if (buffer.format != WINDOW_FORMAT_RGBA_8888 &&
        buffer.format != WINDOW_FORMAT_RGBX_8888) {
        MOVIE_FFLOGE("[ffmpeg] clear surface: buffer format=%d",
                     buffer.format);
        (void)ANativeWindow_unlockAndPost(window);
        ANativeWindow_release(window);
        return;
    }
    if (buffer.bits && buffer.stride > 0 && buffer.width > 0 &&
        buffer.height > 0) {
        for (int y = 0; y < buffer.height; ++y) {
            uint32_t *row = (uint32_t *)((uint8_t *)buffer.bits +
                                         (size_t)y * (size_t)buffer.stride * 4u);
            for (int x = 0; x < buffer.width; ++x)
                row[x] = 0xFF000000u;
        }
        MOVIE_FFLOG("[ffmpeg] clear surface %dx%d stride=%d fmt=%d",
                    buffer.width, buffer.height, buffer.stride,
                    buffer.format);
    }
    (void)ANativeWindow_unlockAndPost(window);
    ANativeWindow_release(window);
}

JNIEXPORT jlong JNICALL
Java_wc3_recomp_MoviePlayer_nativeStartVideo(
        JNIEnv *env, jclass thiz, jstring path, jobject surface, jobject clock,
        jlong start_ms)
{
    (void)thiz;
    if (!env || !path || !surface || !clock)
        return 0;
    (void)pthread_once(&movie_log_once, movie_log_setup);

    const char *path_chars = (*env)->GetStringUTFChars(env, path, NULL);
    if (!path_chars) {
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        return 0;
    }

    MovieVideo *movie = (MovieVideo *)calloc(1, sizeof(*movie));
    if (!movie) {
        (*env)->ReleaseStringUTFChars(env, path, path_chars);
        return 0;
    }
    movie->path = strdup(path_chars);
    (*env)->ReleaseStringUTFChars(env, path, path_chars);
    if (!movie->path)
        goto fail;

    movie->window = ANativeWindow_fromSurface(env, surface);
    if (!movie->window) {
        MOVIE_FFLOGE("[ffmpeg] ANativeWindow_fromSurface failed");
        goto fail;
    }
    if ((*env)->GetJavaVM(env, &movie->vm) != JNI_OK || !movie->vm)
        goto fail;

    movie->clock = (*env)->NewGlobalRef(env, clock);
    if (!movie->clock)
        goto fail;
    jclass clock_class = (*env)->GetObjectClass(env, clock);
    if (!clock_class)
        goto fail;
    movie->clock_position_ms = (*env)->GetMethodID(
            env, clock_class, "positionMs", "()J");
    (*env)->DeleteLocalRef(env, clock_class);
    if (!movie->clock_position_ms)
        goto fail;

    movie->start_us = start_ms > 0 ? (int64_t)start_ms * 1000 : 0;
    atomic_store_explicit(&movie->stop, 0, memory_order_relaxed);
    if (pthread_create(&movie->thread, NULL, video_thread, movie) != 0)
        goto fail;
    movie->thread_started = 1;
    return (jlong)(intptr_t)movie;

fail:
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (movie->clock)
        (*env)->DeleteGlobalRef(env, movie->clock);
    if (movie->window)
        ANativeWindow_release(movie->window);
    free(movie->path);
    free(movie);
    return 0;
}

JNIEXPORT void JNICALL
Java_wc3_recomp_MoviePlayer_nativeStopVideo(
        JNIEnv *env, jclass thiz, jlong handle)
{
    (void)thiz;
    MovieVideo *movie = (MovieVideo *)(intptr_t)handle;
    if (!movie)
        return;

    atomic_store_explicit(&movie->stop, 1, memory_order_release);
    if (movie->thread_started)
        (void)pthread_join(movie->thread, NULL);
    if (movie->window)
        ANativeWindow_release(movie->window);
    if (movie->clock)
        (*env)->DeleteGlobalRef(env, movie->clock);
    free(movie->path);
    free(movie);
}
