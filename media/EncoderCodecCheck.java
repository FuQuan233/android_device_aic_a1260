/* SPDX-License-Identifier: Apache-2.0 */
import android.graphics.ImageFormat;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;

/** Named vendor encoder -> real SDK MediaCodec output. Encoded packets and
 * original I420 inputs are exported for independent host decoding/PSNR.
 * No pixel-quality claim is made by this collector alone. */
public final class EncoderCodecCheck {
    static final int FRAMES=8, FPS=15;
    static final long STEP_US=66667, BASE_US=1000000;
    static void check(boolean yes,String message){if(!yes)throw new AssertionError(message);}
    static final class Config {
        final String name,mime,extension;
        final int width,height,bitrate,profile,level;
        Config(String mode){
            boolean h263=mode.equals("h263")||mode.equals("c2.mt6580.h263.encoder");
            boolean large=mode.equals("mpeg4-720"),minimum=mode.equals("mpeg4-min");
            check(h263||large||minimum||mode.equals("mpeg4")||mode.equals("c2.mt6580.mpeg4.encoder"),
                "mode must be mpeg4, mpeg4-720, mpeg4-min or h263");
            name=h263?"c2.mt6580.h263.encoder":"c2.mt6580.mpeg4.encoder";
            mime=h263?"video/3gpp":"video/mp4v-es";extension=h263?"h263":"m4v";
            width=large?1280:h263?176:128;height=large?720:h263?144:96;
            bitrate=large?4000000:minimum?32000:h263?256000:64000;
            profile=h263?MediaCodecInfo.CodecProfileLevel.H263ProfileBaseline:MediaCodecInfo.CodecProfileLevel.MPEG4ProfileSimple;
            level=h263?MediaCodecInfo.CodecProfileLevel.H263Level40:large?
                MediaCodecInfo.CodecProfileLevel.MPEG4Level6:MediaCodecInfo.CodecProfileLevel.MPEG4Level0;
        }
        MediaFormat format(boolean surface){
            MediaFormat f=MediaFormat.createVideoFormat(mime,width,height);
            f.setInteger(MediaFormat.KEY_BIT_RATE,bitrate);f.setInteger(MediaFormat.KEY_FRAME_RATE,FPS);
            f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL,1);f.setInteger(MediaFormat.KEY_PROFILE,profile);
            f.setInteger(MediaFormat.KEY_LEVEL,level);
            f.setInteger(MediaFormat.KEY_COLOR_FORMAT,surface?MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface:
                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            return f;
        }
        MediaCodec create(boolean surface)throws Exception{
            String automatic=new MediaCodecList(MediaCodecList.REGULAR_CODECS).findEncoderForFormat(format(surface));
            System.out.println("AUTOMATIC_ENCODER="+automatic);check(name.equals(automatic),"automatic hardware encoder selection failed");
            MediaCodec codec=MediaCodec.createByCodecName(name);
            try {
                MediaCodecInfo info=codec.getCodecInfo();
                check(info.isEncoder()&&info.isHardwareAccelerated()&&!info.isSoftwareOnly()&&info.isVendor(),"not a vendor hardware encoder");
                System.out.println("HARDWARE_ENCODER="+info.getName()+" hardware=1 vendor=1 surface="+surface);
                return codec;
            } catch(Throwable t){codec.release();throw t;}
        }
    }
    static long pts(int n){return BASE_US+n*STEP_US;}
    static final class Packet {
        final byte[] bytes;final long pts;final int flags,frame;
        Packet(byte[] b,long p,int f,int n){bytes=b;pts=p;flags=f;frame=n;}
    }
    static final class Collector {
        final Config config;final List<Packet> packets=new ArrayList<>();final HashSet<Long> seen=new HashSet<>();
        byte[] csd=new byte[0];boolean eos;long lastPts=-1;int formatEvents;
        Collector(Config c){config=c;}
        void setCsd(byte[] value){
            if(value.length==0)return;
            check(value.length<=65536,"excessive codec configuration");
            if(csd.length==0||startsWith(value,csd))csd=value;
            else check(startsWith(csd,value),"codec configuration changed during one stream");
        }
        void format(MediaFormat f){
            check(f.getString(MediaFormat.KEY_MIME).equals(config.mime),"output MIME");
            if(f.containsKey(MediaFormat.KEY_WIDTH))check(f.getInteger(MediaFormat.KEY_WIDTH)==config.width,"output width");
            if(f.containsKey(MediaFormat.KEY_HEIGHT))check(f.getInteger(MediaFormat.KEY_HEIGHT)==config.height,"output height");
            ByteArrayOutputStream header=new ByteArrayOutputStream();
            for(int i=0;i<4;i++)if(f.containsKey("csd-"+i)){
                ByteBuffer b=f.getByteBuffer("csd-"+i).duplicate();byte[] data=new byte[b.remaining()];b.get(data);header.write(data,0,data.length);
            }
            setCsd(header.toByteArray());formatEvents++;System.out.println("ENCODER_FORMAT "+f);
        }
        boolean pump(MediaCodec codec,long waitUs){
            MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();int output=codec.dequeueOutputBuffer(info,waitUs);
            if(output==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED){format(codec.getOutputFormat());return true;}
            if(output<0)return false;
            try {
                byte[] data=new byte[info.size];
                if(info.size>0){
                    ByteBuffer out=codec.getOutputBuffer(output);check(out!=null,"null encoded output");
                    check(info.offset>=0&&info.size>=0&&(long)info.offset+info.size<=out.capacity(),"output range");
                    ByteBuffer bytes=out.duplicate();bytes.position(info.offset);bytes.limit(info.offset+info.size);bytes.get(data);
                }
                if((info.flags&MediaCodec.BUFFER_FLAG_CODEC_CONFIG)!=0)setCsd(data);
                else if(data.length>0){
                    long delta=info.presentationTimeUs-BASE_US;
                    check(delta>=0&&delta%STEP_US==0,"unpreserved output PTS "+info.presentationTimeUs);
                    int frame=(int)(delta/STEP_US);
                    check(frame>=0&&frame<FRAMES&&seen.add(info.presentationTimeUs),"unknown or duplicate output frame");
                    check(info.presentationTimeUs>lastPts,"non-monotonic no-B encoder output");lastPts=info.presentationTimeUs;
                    if(packets.isEmpty())check((info.flags&MediaCodec.BUFFER_FLAG_KEY_FRAME)!=0,"first packet is not a keyframe");
                    packets.add(new Packet(data,info.presentationTimeUs,info.flags,frame));
                    System.out.println("ENCODED_PACKET frame="+frame+" pts="+info.presentationTimeUs+" bytes="+data.length+" flags="+info.flags);
                }
                if((info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0)eos=true;
            } finally {codec.releaseOutputBuffer(output,false);}
            return true;
        }
        void complete(){
            check(eos&&seen.size()==FRAMES&&packets.size()==FRAMES,"incomplete encoded stream: packets="+packets.size()+" EOS="+eos);
            check(formatEvents>0,"no output format event");
        }
        void save(String prefix,byte[] reference)throws Exception{
            complete();Path parent=Paths.get(prefix).getParent();if(parent!=null)Files.createDirectories(parent);
            ByteArrayOutputStream encoded=new ByteArrayOutputStream();
            boolean embedded=csd.length>0&&startsWith(packets.get(0).bytes,csd);
            if(!embedded)encoded.write(csd);
            StringBuilder index=new StringBuilder();JSONArray entries=new JSONArray();
            for(Packet p:packets){int offset=encoded.size();encoded.write(p.bytes);index.append(offset).append(' ').append(p.bytes.length).append(' ')
                .append(p.pts).append(' ').append(p.frame).append(' ').append(p.flags).append('\n');
                entries.put(new JSONObject().put("offset",offset).put("bytes",p.bytes.length).put("pts_us",p.pts)
                    .put("input_frame",p.frame).put("flags",p.flags));}
            check(encoded.size()>0,"empty bitstream");
            Files.write(Paths.get(prefix+"."+config.extension),encoded.toByteArray());Files.write(Paths.get(prefix+".csd"),csd);
            Files.write(Paths.get(prefix+".packet-index"),index.toString().getBytes(StandardCharsets.UTF_8));
            if(reference!=null)Files.write(Paths.get(prefix+".i420"),reference);
            JSONObject summary=new JSONObject().put("codec",config.name).put("mime",config.mime)
                .put("width",config.width).put("height",config.height).put("fps",FPS).put("bitrate",config.bitrate)
                .put("frames",FRAMES).put("eos",true).put("csd_bytes",csd.length).put("csd_already_in_first_packet",embedded)
                .put("packet_index_columns",new JSONArray(Arrays.asList("offset","bytes","pts_us","input_frame","flags")))
                .put("frame_identity","input submission index recovered from exact preserved PTS; public MediaCodec does not expose C2/native tokens")
                .put("packets",entries).put("quality_validation","independent host FFmpeg decoding/PSNR still required");
            Files.write(Paths.get(prefix+".json"),summary.toString(2).getBytes(StandardCharsets.UTF_8));
            System.out.println("ENCODER_STREAM_SAVED prefix="+prefix+" frames="+FRAMES+" eos=1 original_PTS=1");
        }
    }
    static boolean startsWith(byte[] data,byte[] prefix){
        if(data.length<prefix.length)return false;for(int i=0;i<prefix.length;i++)if(data[i]!=prefix[i])return false;return true;
    }
    static byte[] referenceFrame(Config c,int frame){
        byte[] data=new byte[c.width*c.height*3/2];int at=0;
        int boxX=frame*(c.width-24)/(FRAMES-1),boxY=frame*(c.height-16)/(FRAMES-1);
        for(int y=0;y<c.height;y++)for(int x=0;x<c.width;x++){
            int luma=16+(x*160/c.width+y*48/c.height+frame*12)%220;
            if(x>=boxX&&x<boxX+24&&y>=boxY&&y<boxY+16)luma=220-frame*8;
            data[at++]=(byte)luma;
        }
        for(int y=0;y<c.height/2;y++)for(int x=0;x<c.width/2;x++)data[at++]=(byte)(80+frame*5+x*96/c.width);
        for(int y=0;y<c.height/2;y++)for(int x=0;x<c.width/2;x++)data[at++]=(byte)(144-frame*3+y*96/c.height);
        return data;
    }
    static void fill(Image image,Config c,byte[] data){
        check(image!=null,"encoder getInputImage returned null");
        try {
            check(image.getFormat()==ImageFormat.YUV_420_888&&image.getWidth()>=c.width&&image.getHeight()>=c.height,"input Image format/dimensions");
            Image.Plane[] planes=image.getPlanes();check(planes.length==3,"input plane count");int at=0;
            for(int plane=0;plane<3;plane++){
                int w=plane==0?c.width:c.width/2,h=plane==0?c.height:c.height/2;
                ByteBuffer out=planes[plane].getBuffer();int start=out.position(),row=planes[plane].getRowStride(),col=planes[plane].getPixelStride();
                check(row>0&&col>0,"input plane stride");
                for(int y=0;y<h;y++)for(int x=0;x<w;x++)out.put(start+y*row+x*col,data[at++]);
            }
        } finally {image.close();}
    }
    static void encode(MediaCodec codec,Config c,String prefix,boolean dataEos)throws Exception{
        Collector output=new Collector(c);ByteArrayOutputStream reference=new ByteArrayOutputStream();
        int sent=0;boolean eosQueued=false;long until=System.nanoTime()+60000000000L;
        while(!output.eos&&System.nanoTime()<until){
            if(!eosQueued){int input=codec.dequeueInputBuffer(10000);if(input>=0){
                if(sent<FRAMES){byte[] bytes=referenceFrame(c,sent);fill(codec.getInputImage(input),c,bytes);reference.write(bytes);
                    int flags=dataEos&&sent==FRAMES-1?MediaCodec.BUFFER_FLAG_END_OF_STREAM:0;
                    codec.queueInputBuffer(input,0,bytes.length,pts(sent),flags);sent++;eosQueued=flags!=0;
                }else{codec.queueInputBuffer(input,0,0,pts(FRAMES),MediaCodec.BUFFER_FLAG_END_OF_STREAM);eosQueued=true;}
            }}
            output.pump(codec,10000);
        }
        check(sent==FRAMES&&eosQueued,"input submission incomplete");output.save(prefix,reference.toByteArray());
    }
    public static void main(String[] args)throws Exception{
        check(args.length==1||args.length==2,"usage PREFIX [mpeg4|mpeg4-720|mpeg4-min|h263|CODEC_NAME]");Config c=new Config(args.length==2?args[1]:"mpeg4");
        MediaCodec codec=c.create(false);
        try {
            codec.configure(c.format(false),null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);codec.start();encode(codec,c,args[0],true);codec.stop();
            codec.configure(c.format(false),null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);codec.start();encode(codec,c,args[0]+"-restart",false);codec.stop();
        } finally {codec.release();}
        codec=c.create(false);
        try {codec.configure(c.format(false),null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);codec.start();encode(codec,c,args[0]+"-recreate",true);codec.stop();}
        finally {codec.release();}
        System.out.println("ANDROID_ENCODER_CONFIRMED codec="+c.name+" frames=24 inputImage=1 original_PTS=1 dataEOS_emptyEOS_restart_recreate=1");
    }
}
