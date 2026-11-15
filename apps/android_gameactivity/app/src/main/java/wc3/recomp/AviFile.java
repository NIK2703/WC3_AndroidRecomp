package wc3.recomp;

import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Minimal RIFF/AVI reader for the Warcraft III campaign movies.
 *
 * <p>The Movies/*.mpq files are not MPQ archives: they are ordinary RIFF AVI
 * files with one MPEG-4 Part 2 video stream and one MP3 audio stream.  Android
 * has no AVI extractor, so this class exposes the two sample lists in file
 * order.  It deliberately understands only the chunks the cinematic files
 * contain; anything else is skipped by its RIFF size.
 *
 * <p>Video samples are in <em>decode</em> order.  The MPEG-4 stream contains
 * B-frames, so a decoder input buffer must be queued in decode order while the
 * presentation timestamps follow display order.  {@link #assignDisplayOrder}
 * performs that mapping once, without touching the sample bytes.
 */
final class AviFile {
    static final int TYPE_I = 0;
    static final int TYPE_P = 1;
    static final int TYPE_B = 2;
    static final int TYPE_UNKNOWN = -1;

    static final class Sample {
        final long offset;
        final int size;
        final int type;
        int display;

        Sample(long offset, int size, int type) {
            this.offset = offset;
            this.size = size;
            this.type = type;
            this.display = -1;
        }
    }

    final int width;
    final int height;
    final int fpsNum;
    final int fpsDen;
    final byte[] csd;
    final List<Sample> video;
    final List<Sample> audio;
    final int maxVideoSize;

    private AviFile(Builder b) {
        width = b.width;
        height = b.height;
        fpsNum = b.fpsNum;
        fpsDen = b.fpsDen;
        csd = b.csd;
        video = b.video;
        audio = b.audio;
        maxVideoSize = b.maxVideoSize;
    }

    long presentationTimeUs(int displayIndex) {
        return (long) displayIndex * 1_000_000L * (long) fpsDen
                / (long) fpsNum;
    }

    static AviFile parse(File file) throws IOException {
        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            long fileLength = raf.length();
            if (fileLength < 12) throw new IOException("short AVI");

            byte[] header = read(raf, 0, 12);
            if (!id(header, 0, "RIFF") || !id(header, 8, "AVI ")) {
                throw new IOException("not a RIFF AVI");
            }

            long riffEnd = 8L + u32(header, 4);
            if (riffEnd < 12L || riffEnd > fileLength) riffEnd = fileLength;

            Builder b = new Builder();
            long pos = 12L;
            while (pos + 8L <= riffEnd) {
                byte[] chunk = read(raf, pos, 8);
                String id = ascii(chunk, 0, 4);
                long size = u32(chunk, 4);
                long body = pos + 8L;
                if (body + size > fileLength) break;

                if ("LIST".equals(id) && size >= 4L) {
                    byte[] type = read(raf, body, 4);
                    String listType = ascii(type, 0, 4);
                    long listBody = body + 4L;
                    long listEnd = body + size;
                    if ("hdrl".equals(listType)) {
                        parseHdrl(raf, listBody, listEnd, b);
                    } else if ("movi".equals(listType)) {
                        parseMovi(raf, listBody, listEnd, b, 0);
                    }
                }
                pos = body + size + (size & 1L);
            }

            if (b.width <= 0 || b.height <= 0) {
                throw new IOException("AVI has no video dimensions");
            }
            if (b.csd == null || b.csd.length == 0 || b.video.isEmpty()) {
                throw new IOException("AVI has no MPEG-4 video stream");
            }
            assignDisplayOrder(b.video);
            return new AviFile(b);
        }
    }

    private static void parseHdrl(RandomAccessFile raf, long start, long end,
                                  Builder b) throws IOException {
        long pos = start;
        while (pos + 8L <= end) {
            byte[] chunk = read(raf, pos, 8);
            String id = ascii(chunk, 0, 4);
            long size = u32(chunk, 4);
            long body = pos + 8L;
            if (body + size > end) break;

            if ("LIST".equals(id) && size >= 4L) {
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
            String id = ascii(chunk, 0, 4);
            long size = u32(chunk, 4);
            long body = pos + 8L;
            if (body + size > end) break;

            if ("strh".equals(id)) {
                strh = read(raf, body, (int) Math.min(size, 4096L));
            } else if ("strf".equals(id)) {
                strf = read(raf, body, (int) Math.min(size, 4096L));
            }
            pos = body + size + (size & 1L);
        }
        if (strh == null || strf == null || strh.length < 28) return;

        String type = ascii(strh, 0, 4);
        if ("vids".equals(type)) {
            int scale = s32(strh, 20);
            int rate = s32(strh, 24);
            if (scale > 0 && rate > 0) {
                b.fpsNum = rate;
                b.fpsDen = scale;
            }
            if (strf.length >= 12) {
                int w = s32(strf, 4);
                int h = s32(strf, 8);
                if (w > 0 && h != 0) {
                    b.width = w;
                    b.height = Math.abs(h);
                }
            }
        }
    }

    private static void parseMovi(RandomAccessFile raf, long start, long end,
                                  Builder b, int depth) throws IOException {
        if (depth > 4) throw new IOException("deep AVI list");

        long pos = start;
        while (pos + 8L <= end) {
            byte[] chunk = read(raf, pos, 8);
            String id = ascii(chunk, 0, 4);
            long size = u32(chunk, 4);
            long body = pos + 8L;
            if (body + size > end) break;

            if ("LIST".equals(id) && size >= 4L) {
                byte[] type = read(raf, body, 4);
                if ("rec".equals(ascii(type, 0, 4))) {
                    parseMovi(raf, body + 4L, body + size, b, depth + 1);
                }
            } else if (isVideoChunk(id)) {
                parseVideoSample(raf, body, size, b);
            } else if (isAudioChunk(id)) {
                if (size > 0L && size <= Integer.MAX_VALUE) {
                    b.audio.add(new Sample(body, (int) size, TYPE_UNKNOWN));
                }
            }
            pos = body + size + (size & 1L);
        }
    }

    private static void parseVideoSample(RandomAccessFile raf, long body,
                                         long size, Builder b)
            throws IOException {
        if (size <= 0L || size > Integer.MAX_VALUE) return;

        int headLength = (int) Math.min(size, 65536L);
        byte[] head = read(raf, body, headLength);
        int vop = findVop(head);

        long sampleOffset = body;
        int sampleSize = (int) size;
        if (b.csd == null) {
            // The first video chunk starts with the MPEG-4 sequence/picture
            // header (VOS/VOL/user data).  MediaCodec wants that part as
            // csd-0, not as the first compressed frame.
            if (vop <= 0) throw new IOException("missing video CSD");
            b.csd = Arrays.copyOf(head, vop);
            sampleOffset = body + vop;
            sampleSize = (int) size - vop;
        }

        int type = TYPE_UNKNOWN;
        if (vop >= 0 && vop + 4 < headLength) {
            int encoded = (head[vop + 4] >>> 6) & 3;
            if (encoded == 0) type = TYPE_I;
            else if (encoded == 1) type = TYPE_P;
            else if (encoded == 2) type = TYPE_B;
        }

        if (sampleSize <= 0) return;
        b.video.add(new Sample(sampleOffset, sampleSize, type));
        if (sampleSize > b.maxVideoSize) b.maxVideoSize = sampleSize;
    }

    /**
     * Map decode order to presentation order for an MPEG-4 stream with
     * B-frames.  The first frame is the initial anchor.  The second frame is
     * held; every following non-B frame is the next anchor.  Before that
     * anchor is presented, all B-frames that precede it are presented.  At
     * end-of-file the remaining B-frames precede the final held anchor.
     */
    private static void assignDisplayOrder(List<Sample> frames) {
        int n = frames.size();
        if (n == 0) return;

        // A stream that begins with B is malformed for this simple mapping;
        // keep file order rather than inventing an anchor that is not there.
        if (n > 1 && frames.get(1).type == TYPE_B) {
            for (int i = 0; i < n; i++) frames.get(i).display = i;
            return;
        }

        int next = 0;
        frames.get(0).display = next++;
        if (n == 1) return;

        int held = 1;
        List<Integer> pending = new ArrayList<>();
        for (int i = 2; i < n; i++) {
            Sample s = frames.get(i);
            if (s.type == TYPE_B) {
                pending.add(i);
            } else {
                for (int index : pending) frames.get(index).display = next++;
                pending.clear();
                frames.get(held).display = next++;
                held = i;
            }
        }
        for (int index : pending) frames.get(index).display = next++;
        frames.get(held).display = next++;

        if (next != n) {
            for (int i = 0; i < n; i++) frames.get(i).display = i;
        }
    }

    private static int findVop(byte[] data) {
        for (int i = 0; i + 4 <= data.length; i++) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1
                    && (data[i + 3] & 0xFF) == 0xB6) {
                return i;
            }
        }
        return -1;
    }

    private static boolean isVideoChunk(String id) {
        return id.length() == 4 && id.charAt(0) == '0' && id.charAt(1) == '0'
                && id.charAt(2) == 'd';
    }

    private static boolean isAudioChunk(String id) {
        return id.length() == 4 && id.charAt(0) == '0' && id.charAt(1) == '1'
                && id.charAt(2) == 'w' && id.charAt(3) == 'b';
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
        byte[] csd;
        final List<Sample> video = new ArrayList<>();
        final List<Sample> audio = new ArrayList<>();
        int maxVideoSize;
    }
}
