package wc3.recomp;

import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

/**
 * Minimal RIFF/AVI header reader for the Warcraft III campaign movies.
 *
 * <p>The Movies/*.mpq files are ordinary RIFF AVI files, not MPQ archives.
 * Media3Avi owns AVI audio and the playback clock; the video stream is decoded
 * by FFmpeg.  This class only reads the video stream's dimensions and frame
 * rate so the host can size its SurfaceView before playback starts.  It never
 * walks the {@code movi} list and therefore does not read or allocate movie
 * frames.
 */
final class AviFile {
    final int width;
    final int height;
    final int fpsNum;
    final int fpsDen;

    private AviFile(int width, int height, int fpsNum, int fpsDen) {
        this.width = width;
        this.height = height;
        this.fpsNum = fpsNum;
        this.fpsDen = fpsDen;
    }

    static AviFile parse(File file) throws IOException {
        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            long fileLength = raf.length();
            if (fileLength < 12) throw new IOException("short AVI");

            byte[] header = read(raf, 0, 12);
            if (!id(header, 0, "RIFF") || !id(header, 8, "AVI ")) {
                throw new IOException("not a RIFF AVI");
            }

            long riffEnd = 8L + (u32(header, 4) & 0xFFFFFFFFL);
            if (riffEnd < 12L || riffEnd > fileLength) riffEnd = fileLength;

            Builder b = new Builder();
            long pos = 12L;
            while (pos + 8L <= riffEnd) {
                byte[] chunk = read(raf, pos, 8);
                String chunkId = ascii(chunk, 0, 4);
                long size = u32(chunk, 4) & 0xFFFFFFFFL;
                long body = pos + 8L;
                if (body + size > fileLength) break;

                if ("LIST".equals(chunkId) && size >= 4L) {
                    byte[] type = read(raf, body, 4);
                    if ("hdrl".equals(ascii(type, 0, 4))) {
                        parseHdrl(raf, body + 4L, body + size, b);
                    }
                }
                pos = body + size + (size & 1L);
            }

            if (b.width <= 0 || b.height <= 0) {
                throw new IOException("AVI has no video dimensions");
            }
            return new AviFile(b.width, b.height, b.fpsNum, b.fpsDen);
        }
    }

    private static void parseHdrl(RandomAccessFile raf, long start, long end,
                                  Builder b) throws IOException {
        long pos = start;
        while (pos + 8L <= end) {
            byte[] chunk = read(raf, pos, 8);
            String chunkId = ascii(chunk, 0, 4);
            long size = u32(chunk, 4) & 0xFFFFFFFFL;
            long body = pos + 8L;
            if (body + size > end) break;

            if ("LIST".equals(chunkId) && size >= 4L) {
                byte[] type = read(raf, body, 4);
                if ("strl".equals(ascii(type, 0, 4))) {
                    parseStrl(raf, body + 4L, body + size, b);
                }
            }
            pos = body + size + (size & 1L);
        }
    }

    private static void parseStrl(RandomAccessFile raf, long start, long end,
                                  Builder b) throws IOException {
        byte[] strh = null;
        byte[] strf = null;
        long pos = start;
        while (pos + 8L <= end) {
            byte[] chunk = read(raf, pos, 8);
            String chunkId = ascii(chunk, 0, 4);
            long size = u32(chunk, 4) & 0xFFFFFFFFL;
            long body = pos + 8L;
            if (body + size > end) break;

            if ("strh".equals(chunkId)) {
                strh = read(raf, body, (int) Math.min(size, 4096L));
            } else if ("strf".equals(chunkId)) {
                strf = read(raf, body, (int) Math.min(size, 4096L));
            }
            pos = body + size + (size & 1L);
        }
        if (strh == null || strf == null || strh.length < 28
                || strf.length < 12) {
            return;
        }
        if (!"vids".equals(ascii(strh, 0, 4))) return;

        int scale = s32(strh, 20);
        int rate = s32(strh, 24);
        if (scale > 0 && rate > 0) {
            b.fpsNum = rate;
            b.fpsDen = scale;
        }

        int w = s32(strf, 4);
        int h = s32(strf, 8);
        if (w > 0 && h != 0) {
            b.width = w;
            b.height = Math.abs(h);
        }
    }

    private static byte[] read(RandomAccessFile raf, long offset, int count)
            throws IOException {
        if (count < 0) throw new IOException("negative read");
        byte[] data = new byte[count];
        raf.seek(offset);
        try {
            raf.readFully(data);
        } catch (EOFException e) {
            throw new IOException("truncated AVI");
        }
        return data;
    }

    private static int u32(byte[] data, int offset) {
        return (data[offset] & 0xFF)
                | ((data[offset + 1] & 0xFF) << 8)
                | ((data[offset + 2] & 0xFF) << 16)
                | ((data[offset + 3] & 0xFF) << 24);
    }

    private static int s32(byte[] data, int offset) {
        return u32(data, offset);
    }

    private static boolean id(byte[] data, int offset, String value) {
        if (offset + 4 > data.length || value.length() != 4) return false;
        for (int i = 0; i < 4; i++) {
            if ((data[offset + i] & 0xFF) != value.charAt(i)) return false;
        }
        return true;
    }

    private static String ascii(byte[] data, int offset, int length) {
        return new String(data, offset, length, StandardCharsets.US_ASCII);
    }

    private static final class Builder {
        int width;
        int height;
        int fpsNum = 24;
        int fpsDen = 1;
    }
}
