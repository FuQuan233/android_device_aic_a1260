/* SPDX-License-Identifier: Apache-2.0 */
import android.graphics.ImageFormat;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Explicitly named real MediaCodec qualification, not a framework bypass. */
public final class MediaCodecCheck {
    static final String NAME = "c2.mt6580.avc.decoder";
    static int WIDTH = 128, HEIGHT = 96, FRAME = WIDTH * HEIGHT * 3 / 2;
    static byte[] reference;
    static List<byte[]> units = new ArrayList<>();
    static byte[] sps, pps;
    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    static int prefix(byte[] data, int at) {
        if (at + 4 <= data.length && data[at] == 0 && data[at+1] == 0 &&
            data[at+2] == 0 && data[at+3] == 1) return 4;
        if (at + 3 <= data.length && data[at] == 0 && data[at+1] == 0 && data[at+2] == 1) return 3;
        return 0;
    }
    static MediaFormat format() {
        MediaFormat f = MediaFormat.createVideoFormat("video/avc", WIDTH, HEIGHT);
        f.setByteBuffer("csd-0", ByteBuffer.wrap(sps));
        f.setByteBuffer("csd-1", ByteBuffer.wrap(pps));
        f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline);
        f.setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel4);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        return f;
    }
    static void pixels(Image image, int index) {
        check(image != null, "getOutputImage returned null");
        try {
            check(image.getFormat() == ImageFormat.YUV_420_888, "unexpected Image format");
            check(image.getWidth() >= WIDTH && image.getHeight() >= HEIGHT, "small Image");
            Image.Plane[] planes = image.getPlanes();
            check(planes.length == 3, "plane count");
            int referenceAt = index * FRAME;
            for (int plane = 0; plane < 3; plane++) {
                ByteBuffer data = planes[plane].getBuffer();
                int w = plane == 0 ? WIDTH : WIDTH/2;
                int h = plane == 0 ? HEIGHT : HEIGHT/2;
                int start = data.position();
                int row = planes[plane].getRowStride(), col = planes[plane].getPixelStride();
                for (int y=0; y<h; y++) for (int x=0; x<w; x++) {
                    int actual = data.get(start + y*row + x*col) & 255;
                    int expected = reference[referenceAt++] & 255;
                    if (actual != expected) throw new AssertionError("pixel mismatch frame=" + index +
                        " plane=" + plane + " x=" + x + " y=" + y + " actual=" + actual + " expected=" + expected);
                }
            }
        } finally { image.close(); }
    }
    static void decode(MediaCodec codec, String phase, boolean dataEos) throws Exception {
        int sent = 0, received = 0;
        boolean eosSent = false, eosSeen = false;
        long deadline = System.nanoTime() + 30000000000L;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        while (!eosSeen && System.nanoTime() < deadline) {
            if (!eosSent) {
                int input = codec.dequeueInputBuffer(10000);
                if (input >= 0) {
                    ByteBuffer buffer = codec.getInputBuffer(input);
                    buffer.clear();
                    int size = 0, flags = 0;
                    if (sent < units.size()) {
                        byte[] au = units.get(sent); buffer.put(au); size = au.length;
                        if (dataEos && sent == units.size()-1) { flags=MediaCodec.BUFFER_FLAG_END_OF_STREAM; eosSent=true; }
                        codec.queueInputBuffer(input, 0, size, sent*42000L, flags); sent++;
                    } else {
                        eosSent=true;
                        codec.queueInputBuffer(input, 0, 0, sent*42000L, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                    }
                }
            }
            int output = codec.dequeueOutputBuffer(info, 10000);
            if (output == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat f = codec.getOutputFormat();
                System.out.println("FORMAT " + phase + " " + f);
                check(f.getInteger(MediaFormat.KEY_WIDTH)==WIDTH && f.getInteger(MediaFormat.KEY_HEIGHT)==HEIGHT,
                    "output dimensions");
            } else if (output >= 0) {
                try {
                    if (info.size > 0) {
                        check(received < 8, "too many frames");
                        check(info.presentationTimeUs == received*42000L, "PTS mismatch");
                        pixels(codec.getOutputImage(output), received);
                        received++;
                    }
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) eosSeen=true;
                } finally { codec.releaseOutputBuffer(output, false); }
            }
        }
        check(eosSeen && received==8, "decode incomplete " + phase + " frames=" + received + " eos=" + eosSeen);
        System.out.println("MEDIACODEC_FRAME_MATCH phase=" + phase + " frames=" + received + " eos=1 pts=1");
    }
    public static void main(String[] args) throws Exception {
        check(args.length==2 || args.length==4, "usage H264 REFERENCE_I420 [WIDTH HEIGHT]");
        if (args.length==4) {
            WIDTH=Integer.parseInt(args[2]); HEIGHT=Integer.parseInt(args[3]);
            check(WIDTH>=16 && WIDTH<=1280 && HEIGHT>=16 && HEIGHT<=720 &&
                (WIDTH%2)==0 && (HEIGHT%2)==0, "fixture dimensions");
            FRAME=WIDTH*HEIGHT*3/2;
        }
        byte[] stream = Files.readAllBytes(Paths.get(args[0]));
        reference = Files.readAllBytes(Paths.get(args[1]));
        check(reference.length==8*FRAME, "reference size");
        for (int at=0; at<stream.length;) {
            int pre = prefix(stream, at);
            check(pre>0 && at+pre<stream.length, "invalid Annex B");
            int end=at+pre+1; while(end<stream.length && prefix(stream,end)==0)end++;
            byte[] unit=Arrays.copyOfRange(stream,at,end);
            int type=stream[at+pre]&31;
            if(type==7)sps=unit; else if(type==8)pps=unit; else if(type==1||type==5)units.add(unit);
            at=end;
        }
        check(sps!=null && pps!=null && units.size()==8, "fixture NALs");
        boolean listed=false, software=false;
        for (MediaCodecInfo info:new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
            if (info.getName().equals(NAME)) {
                check(info.isHardwareAccelerated() && !info.isSoftwareOnly() && info.isVendor(), "wrong codec flags");
                System.out.println("LISTED " + info.getName() + " hardware=1 software=0 vendor=1"); listed=true;
            }
            if(info.getName().equals("c2.android.avc.decoder"))software=true;
        }
        check(listed && software, "hardware codec or software fallback absent");
        String selected = new MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(format());
        check(NAME.equals(selected), "automatic decoder selection: " + selected);
        System.out.println("AUTOMATIC_DECODER=" + selected);
        for (int round=0; round<2; round++) {
            MediaCodec codec=MediaCodec.createByCodecName(NAME);
            try {
                codec.configure(format(), null, null, 0); codec.start();
                decode(codec, "create"+round+"-dataEOS", true);
                codec.flush();
                decode(codec, "flush"+round+"-emptyEOS", false);
                codec.stop();
                codec.configure(format(), null, null, 0); codec.start();
                decode(codec, "restart"+round, true);
                codec.stop();
            } finally { codec.release(); }
        }
        System.out.println("ANDROID_MEDIACODEC_HARDWARE_CONFIRMED frames=48 exact_I420=1 EOS_flush_restart_release=1");
    }
}
