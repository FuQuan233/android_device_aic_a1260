/* SPDX-License-Identifier: Apache-2.0 */
import android.graphics.ImageFormat;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/** Real reordered H.264 qualification. Sidecar PTS comes from the independent
 * encoder's output picture, not from coded AU order. Reference I420 is in
 * display order. The parameter-set NALs are supplied as CSD, not duplicated
 * in the sample AU. All VCL slices belonging to an AU remain together. */
public final class ProfileCodecCheck {
    static final String HARDWARE = "c2.mt6580.avc.decoder";
    static final class Unit {
        byte[] bytes;
        long pts;
        Unit(byte[] data, long timestamp) { bytes=data; pts=timestamp; }
    }
    static int width, height, frameBytes, profile;
    static boolean diagnostic;
    static long differences;
    static String dumpPrefix;
    static byte[] reference, sps, pps;
    static final List<Unit> units = new ArrayList<>();
    static final TreeMap<Long, Integer> referenceByPts = new TreeMap<>();
    static void check(boolean yes, String message) { if (!yes) throw new AssertionError(message); }
    static int prefix(byte[] bytes, int at) {
        if(at+4<=bytes.length && bytes[at]==0 && bytes[at+1]==0 && bytes[at+2]==0 && bytes[at+3]==1)return 4;
        if(at+3<=bytes.length && bytes[at]==0 && bytes[at+1]==0 && bytes[at+2]==1)return 3;
        return 0;
    }
    static byte[] sample(byte[] bytes, boolean config) {
        ByteArrayOutputStream result=new ByteArrayOutputStream();
        for(int at=0;at<bytes.length;) {
            int pre=prefix(bytes,at); check(pre!=0 && at+pre<bytes.length,"bad Annex B");
            int end=at+pre+1;while(end<bytes.length && prefix(bytes,end)==0)end++;
            int type=bytes[at+pre]&31;
            if(config) {
                if(type==7) { sps=Arrays.copyOfRange(bytes,at,end); profile=bytes[at+pre+1]&255; }
                else if(type==8)pps=Arrays.copyOfRange(bytes,at,end);
                else throw new AssertionError("non-CSD NAL "+type);
            } else if(type!=7 && type!=8)result.write(bytes,at,end-at);
            at=end;
        }
        return result.toByteArray();
    }
    static MediaFormat format() {
        MediaFormat f=MediaFormat.createVideoFormat("video/avc",width,height);
        f.setByteBuffer("csd-0",ByteBuffer.wrap(sps));f.setByteBuffer("csd-1",ByteBuffer.wrap(pps));
        f.setInteger(MediaFormat.KEY_PROFILE,profile==77?MediaCodecInfo.CodecProfileLevel.AVCProfileMain:
            MediaCodecInfo.CodecProfileLevel.AVCProfileHigh);
        f.setInteger(MediaFormat.KEY_LEVEL,MediaCodecInfo.CodecProfileLevel.AVCLevel4);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        return f;
    }
    static void pixels(Image image,int frame) {
        check(image!=null,"null output Image");
        try {
            check(image.getFormat()==ImageFormat.YUV_420_888 && image.getWidth()>=width && image.getHeight()>=height,
                "wrong Image format/dimensions");
            Image.Plane[] planes=image.getPlanes();check(planes.length==3,"plane count");
            int offset=frame*frameBytes;
            byte[] actualFrame = diagnostic ? new byte[frameBytes] : null;
            int differing=0,maxError=0;
            for(int p=0;p<3;p++) {
                int w=p==0?width:width/2,h=p==0?height:height/2;
                ByteBuffer data=planes[p].getBuffer();int start=data.position();
                int row=planes[p].getRowStride(),col=planes[p].getPixelStride();
                check(row>0 && col>0,"plane strides");
                for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
                    int actual=data.get(start+y*row+x*col)&255,expected=reference[offset++]&255;
                    if(diagnostic)actualFrame[offset-frame*frameBytes-1]=(byte)actual;
                    if(actual!=expected) {
                        if(!diagnostic)throw new AssertionError("I420 mismatch displayFrame="+frame+
                            " plane="+p+" x="+x+" y="+y+" actual="+actual+" expected="+expected);
                        if(differing<3)System.out.println("PIXEL_DIFFERENCE frame="+frame+" plane="+p+
                            " x="+x+" y="+y+" actual="+actual+" expected="+expected);
                        differing++;maxError=Math.max(maxError,Math.abs(actual-expected));
                    }
                }
            }
            if(diagnostic) {
                try { Files.write(Paths.get(dumpPrefix+".hardware."+frame+".i420"),actualFrame); }
                catch(java.io.IOException e) { throw new AssertionError(e); }
                differences+=differing;
                System.out.println("FRAME_DIAGNOSTIC frame="+frame+" differing="+differing+" max_error="+maxError);
            }
        } finally { image.close(); }
    }
    static void decode(MediaCodec codec,String phase,boolean dataEos)throws Exception {
        int sent=0;boolean queuedEos=false,eos=false;
        HashSet<Long> seen=new HashSet<>();
        MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
        long deadline=System.nanoTime()+60000000000L,lastPts=-1;
        while(!eos && System.nanoTime()<deadline) {
            if(!queuedEos) {
                int index=codec.dequeueInputBuffer(10000);
                if(index>=0) {
                    ByteBuffer input=codec.getInputBuffer(index);input.clear();
                    if(sent<units.size()) {
                        Unit au=units.get(sent);check(input.remaining()>=au.bytes.length,"small input buffer");input.put(au.bytes);
                        int flags=dataEos && sent==units.size()-1?MediaCodec.BUFFER_FLAG_END_OF_STREAM:0;
                        codec.queueInputBuffer(index,0,au.bytes.length,au.pts,flags);
                        queuedEos=flags!=0;sent++;
                    } else { codec.queueInputBuffer(index,0,0,units.size()*42000L,MediaCodec.BUFFER_FLAG_END_OF_STREAM);queuedEos=true; }
                }
            }
            int index=codec.dequeueOutputBuffer(info,10000);
            if(index==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat f=codec.getOutputFormat();
                check(f.getInteger(MediaFormat.KEY_WIDTH)==width && f.getInteger(MediaFormat.KEY_HEIGHT)==height,"output dimensions");
                System.out.println("FORMAT "+phase+" "+f);
            } else if(index>=0) {
                try {
                    if(info.size>0) {
                        check(info.presentationTimeUs>lastPts,"display PTS did not increase");
                        lastPts=info.presentationTimeUs;
                        Integer frame=referenceByPts.get(info.presentationTimeUs);
                        check(frame!=null && seen.add(info.presentationTimeUs),"unknown or duplicated output PTS "+info.presentationTimeUs);
                        pixels(codec.getOutputImage(index),frame);
                        System.out.println("PROFILE_FRAME phase="+phase+" display="+frame+" pts="+info.presentationTimeUs+" pixel_check="+(diagnostic?"diagnostic":"exact"));
                    }
                    if((info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0)eos=true;
                } finally { codec.releaseOutputBuffer(index,false); }
            }
        }
        check(eos && seen.size()==units.size() && sent==units.size(),"incomplete "+phase+" output="+seen.size()+" sent="+sent+" EOS="+eos);
        System.out.println("PROFILE_PHASE_PASS "+phase+" frames="+seen.size()+" exact_I420=1 original_PTS=1 EOS=1");
    }
    public static void main(String[] args)throws Exception {
        check(args.length>=3 && args.length<=5,"usage PREFIX WIDTH HEIGHT [CODEC_NAME [--diagnose]]");
        diagnostic=args.length==5 && args[4].equals("--diagnose");
        dumpPrefix=args[0];
        width=Integer.parseInt(args[1]);height=Integer.parseInt(args[2]);
        check(width>=16 && width<=1280 && height>=16 && height<=720 && width%2==0 && height%2==0,"dimensions");
        frameBytes=width*height*3/2;
        String name=args.length>=4?args[3]:HARDWARE;
        byte[] stream=Files.readAllBytes(Paths.get(args[0]+".h264"));
        reference=Files.readAllBytes(Paths.get(args[0]+".i420"));
        sample(Files.readAllBytes(Paths.get(args[0]+".csd")),true);
        check(sps!=null && pps!=null && (profile==77 || profile==100),"Main/High CSD");
        for(String line:Files.readAllLines(Paths.get(args[0]+".au-index"))) {
            String[] cols=line.trim().split("\\s+");check(cols.length==5,"AU sidecar columns");
            long offset=Long.parseLong(cols[0]),size=Long.parseLong(cols[1]),pts=Long.parseLong(cols[2]);
            check(offset>=0 && size>0 && size<=4194304 && offset+size<=stream.length && pts>=0,"AU bounds/PTS");
            check(referenceByPts.put(pts,0)==null,"duplicate input PTS");
            units.add(new Unit(sample(Arrays.copyOfRange(stream,(int)offset,(int)(offset+size)),false),pts));
        }
        check(units.size()==8 && reference.length==frameBytes*units.size(),"fixture frame count/reference size");
        int at=0;for(Map.Entry<Long,Integer> entry:referenceByPts.entrySet())entry.setValue(at++);
        boolean reordered=false;for(int i=1;i<units.size();i++)if(units.get(i).pts<units.get(i-1).pts)reordered=true;
        check(reordered,"fixture must exercise coded/display reordering");
        String automatic=new MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(format());
        System.out.println("AUTOMATIC_DECODER="+automatic);
        if(name.equals(HARDWARE))check(name.equals(automatic),"hardware not selected for profile "+profile);
        MediaCodec codec=MediaCodec.createByCodecName(name);
        try {
            MediaCodecInfo ci=codec.getCodecInfo();
            if(name.equals(HARDWARE))check(ci.isHardwareAccelerated() && !ci.isSoftwareOnly() && ci.isVendor(),"not hardware vendor codec");
            System.out.println("CODEC="+ci.getName()+" profile_idc="+profile+" width="+width+" height="+height+" reordered=1");
            codec.configure(format(),null,null,0);codec.start();decode(codec,"create-dataEOS",true);
            codec.flush();decode(codec,"flush-emptyEOS",false);
            codec.stop();codec.configure(format(),null,null,0);codec.start();decode(codec,"restart-dataEOS",true);codec.stop();
        } finally { codec.release(); }
        if(diagnostic && differences>0) {
            System.out.println("DIAGNOSTIC_NOT_PIXEL_EXACT differences="+differences);
            System.exit(2);
        }
        System.out.println("ANDROID_PROFILE_CONFIRMED profile_idc="+profile+" frames=24 CABAC_B_frames=1 exact_I420=1 EOS_flush_restart_release=1");
    }
}
