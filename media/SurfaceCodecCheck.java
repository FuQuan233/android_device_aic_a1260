/* SPDX-License-Identifier: Apache-2.0 */
import android.graphics.SurfaceTexture;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.opengl.*;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.CRC32;

/** Exercise hardware MediaCodec Surface -> external texture -> actual GLES
 * readback. Pixel-exact I420 is separately verified by MediaCodecCheck. */
public final class SurfaceCodecCheck {
    static void check(boolean ok, String message) { if(!ok)throw new AssertionError(message); }
    static int shader(int type, String source) {
        int shader=GLES20.glCreateShader(type);GLES20.glShaderSource(shader,source);GLES20.glCompileShader(shader);
        int[] ok={0};GLES20.glGetShaderiv(shader,GLES20.GL_COMPILE_STATUS,ok,0);
        check(ok[0]!=0,GLES20.glGetShaderInfoLog(shader));return shader;
    }
    static FloatBuffer floats(float[] values) {
        FloatBuffer b=ByteBuffer.allocateDirect(values.length*4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        b.put(values).position(0);return b;
    }
    public static void main(String[] args) throws Exception {
        check(args.length==1,"usage AnnexB_H264");
        byte[] stream=Files.readAllBytes(Paths.get(args[0]));
        List<byte[]> units=new ArrayList<>();byte[] sps=null,pps=null;
        for(int at=0;at<stream.length;) {
            int pre=MediaCodecCheck.prefix(stream,at);check(pre>0,"NAL framing");
            int end=at+pre+1;while(end<stream.length&&MediaCodecCheck.prefix(stream,end)==0)end++;
            byte[] unit=Arrays.copyOfRange(stream,at,end);int type=stream[at+pre]&31;
            if(type==7)sps=unit;else if(type==8)pps=unit;else if(type==1||type==5)units.add(unit);at=end;
        }
        check(sps!=null&&pps!=null&&units.size()==8,"fixture");
        EGLDisplay display=EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version=new int[2];check(EGL14.eglInitialize(display,version,0,version,1),"eglInitialize");
        EGLConfig[] configs=new EGLConfig[1];int[] count=new int[1];
        int[] attrs={EGL14.EGL_RENDERABLE_TYPE,EGL14.EGL_OPENGL_ES2_BIT,EGL14.EGL_SURFACE_TYPE,EGL14.EGL_PBUFFER_BIT,
            EGL14.EGL_RED_SIZE,8,EGL14.EGL_GREEN_SIZE,8,EGL14.EGL_BLUE_SIZE,8,EGL14.EGL_ALPHA_SIZE,8,EGL14.EGL_NONE};
        check(EGL14.eglChooseConfig(display,attrs,0,configs,0,1,count,0)&&count[0]>0,"eglChooseConfig");
        EGLContext context=EGL14.eglCreateContext(display,configs[0],EGL14.EGL_NO_CONTEXT,
            new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION,2,EGL14.EGL_NONE},0);
        EGLSurface pbuffer=EGL14.eglCreatePbufferSurface(display,configs[0],
            new int[]{EGL14.EGL_WIDTH,128,EGL14.EGL_HEIGHT,96,EGL14.EGL_NONE},0);
        check(EGL14.eglMakeCurrent(display,pbuffer,pbuffer,context),"eglMakeCurrent");
        System.out.println("GPU_RENDERER="+GLES20.glGetString(GLES20.GL_RENDERER));
        int[] texture={0};GLES20.glGenTextures(1,texture,0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,texture[0]);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_MIN_FILTER,GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_MAG_FILTER,GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_WRAP_S,GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_WRAP_T,GLES20.GL_CLAMP_TO_EDGE);
        SurfaceTexture image=new SurfaceTexture(texture[0]);image.setDefaultBufferSize(128,96);
        HandlerThread callbacks=new HandlerThread("codec-surface-callback");callbacks.start();
        AtomicInteger available=new AtomicInteger();Object lock=new Object();
        image.setOnFrameAvailableListener(t->{synchronized(lock){available.incrementAndGet();lock.notifyAll();}},new Handler(callbacks.getLooper()));
        Surface surface=new Surface(image);
        String vs="attribute vec2 position;attribute vec2 uv;uniform mat4 matrix;varying vec2 texcoord;void main(){gl_Position=vec4(position,0.,1.);texcoord=(matrix*vec4(uv,0.,1.)).xy;}";
        String fs="#extension GL_OES_EGL_image_external : require\nprecision mediump float;varying vec2 texcoord;uniform samplerExternalOES video;void main(){gl_FragColor=texture2D(video,texcoord);}";
        int program=GLES20.glCreateProgram();GLES20.glAttachShader(program,shader(GLES20.GL_VERTEX_SHADER,vs));
        GLES20.glAttachShader(program,shader(GLES20.GL_FRAGMENT_SHADER,fs));GLES20.glLinkProgram(program);
        int[] linked={0};GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,linked,0);check(linked[0]!=0,"GL program link");
        GLES20.glUseProgram(program);
        int position=GLES20.glGetAttribLocation(program,"position"),uv=GLES20.glGetAttribLocation(program,"uv");
        GLES20.glEnableVertexAttribArray(position);GLES20.glEnableVertexAttribArray(uv);
        int[] vbo={0};GLES20.glGenBuffers(1,vbo,0);GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER,vbo[0]);
        FloatBuffer vertices=floats(new float[]{-1,-1,0,0,1,-1,1,0,-1,1,0,1,1,1,1,1});
        GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER,64,vertices,GLES20.GL_STATIC_DRAW);
        GLES20.glVertexAttribPointer(position,2,GLES20.GL_FLOAT,false,16,0);
        GLES20.glVertexAttribPointer(uv,2,GLES20.GL_FLOAT,false,16,8);
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program,"video"),0);
        GLES20.glViewport(0,0,128,96);
        ByteBuffer pixels=ByteBuffer.allocateDirect(128*96*4);
        Set<Long> hashes=new HashSet<>();
        MediaCodec codec=MediaCodec.createByCodecName(MediaCodecCheck.NAME);
        MediaFormat format=MediaFormat.createVideoFormat("video/avc",128,96);
        format.setByteBuffer("csd-0",ByteBuffer.wrap(sps));format.setByteBuffer("csd-1",ByteBuffer.wrap(pps));
        try {
            codec.configure(format,surface,null,0);codec.start();
            int sent=0,received=0,consumed=0;boolean eosSent=false,eosSeen=false;
            long deadline=System.nanoTime()+30000000000L;
            MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            while(!eosSeen&&System.nanoTime()<deadline) {
                if(!eosSent) {
                    int index=codec.dequeueInputBuffer(10000);
                    if(index>=0) {
                        ByteBuffer input=codec.getInputBuffer(index);input.clear();
                        byte[] unit=units.get(sent);input.put(unit);
                        boolean last=sent==units.size()-1;
                        codec.queueInputBuffer(index,0,unit.length,sent*42000L,last?MediaCodec.BUFFER_FLAG_END_OF_STREAM:0);
                        sent++;eosSent=last;
                    }
                }
                int index=codec.dequeueOutputBuffer(info,10000);
                if(index>=0) {
                    boolean render=info.size>0;
                    check(!render||info.presentationTimeUs==received*42000L,"Surface PTS");
                    codec.releaseOutputBuffer(index,render);
                    if(render) {
                        synchronized(lock) {long until=System.nanoTime()+3000000000L;
                            while(available.get()<=consumed&&System.nanoTime()<until)lock.wait(50);}
                        check(available.get()>consumed,"Surface frame callback timeout");consumed=available.get();
                        image.updateTexImage();
                        check(image.getTimestamp()==received*42000000L,"SurfaceTexture timestamp");
                        float[] matrix=new float[16];image.getTransformMatrix(matrix);
                        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program,"matrix"),1,false,matrix,0);
                        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP,0,4);
                        pixels.clear();GLES20.glReadPixels(0,0,128,96,GLES20.GL_RGBA,GLES20.GL_UNSIGNED_BYTE,pixels);
                        check(GLES20.glGetError()==GLES20.GL_NO_ERROR,"GLES external image/readback");
                        CRC32 crc=new CRC32();int min=255,max=0;
                        for(int i=0;i<pixels.capacity();i++){int v=pixels.get(i)&255;crc.update(v);if(i%4!=3){min=Math.min(min,v);max=Math.max(max,v);}}
                        check(max-min>32,"Surface output blank/uniform");
                        hashes.add(crc.getValue());received++;
                        System.out.println("SURFACE_FRAME n="+received+" crc="+Long.toHexString(crc.getValue()));
                    }
                    eosSeen=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;
                }
            }
            check(received==8&&eosSeen&&hashes.size()>=6,"Surface frames/EOS/readback did not advance");
            codec.stop();
            System.out.println("MEDIACODEC_SURFACE_GPU_CONFIRMED frames=8 EOS=1 timestamp=1 moving_readback=1");
        } finally {
            codec.release();surface.release();image.release();callbacks.quitSafely();callbacks.join();
            EGL14.eglMakeCurrent(display,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroySurface(display,pbuffer);EGL14.eglDestroyContext(display,context);EGL14.eglTerminate(display);
        }
    }
}
