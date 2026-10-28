/* SPDX-License-Identifier: Apache-2.0 */
import android.media.MediaCodec;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.view.Surface;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;

/** Actual GLES window -> MediaCodec input Surface -> named vendor encoder.
 * Encoded bytes and expected uniform RGB means are checked independently on
 * the host. A Surface merely being created is not considered success. */
public final class SurfaceEncoderCheck {
    static final int RECORDABLE_ANDROID=0x3142;
    static final float[][] COLORS={{.80f,.10f,.10f},{.10f,.75f,.15f},{.12f,.15f,.80f},{.75f,.70f,.10f},
        {.10f,.70f,.70f},{.65f,.10f,.70f},{.35f,.35f,.35f},{.80f,.80f,.80f}};
    static void check(boolean yes,String message){EncoderCodecCheck.check(yes,message);}
    public static void main(String[] args)throws Exception{
        check(args.length==1||args.length==2,"usage PREFIX [mpeg4|h263|CODEC_NAME]");
        EncoderCodecCheck.Config config=new EncoderCodecCheck.Config(args.length==2?args[1]:"mpeg4");
        MediaCodec codec=config.create(true);Surface input=null;
        EGLDisplay display=EGL14.EGL_NO_DISPLAY;EGLContext context=EGL14.EGL_NO_CONTEXT;EGLSurface window=EGL14.EGL_NO_SURFACE;
        AtomicReference<Throwable> failure=new AtomicReference<>();AtomicBoolean running=new AtomicBoolean(true);
        EncoderCodecCheck.Collector output=new EncoderCodecCheck.Collector(config);Thread drain=null;
        try {
            codec.configure(config.format(true),null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);input=codec.createInputSurface();
            display=EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);int[] version=new int[2];
            check(display!=EGL14.EGL_NO_DISPLAY&&EGL14.eglInitialize(display,version,0,version,1),"eglInitialize");
            EGLConfig[] formats=new EGLConfig[1];int[] count=new int[1];
            int[] attributes={EGL14.EGL_RED_SIZE,8,EGL14.EGL_GREEN_SIZE,8,EGL14.EGL_BLUE_SIZE,8,EGL14.EGL_ALPHA_SIZE,8,
                EGL14.EGL_RENDERABLE_TYPE,EGL14.EGL_OPENGL_ES2_BIT,EGL14.EGL_SURFACE_TYPE,EGL14.EGL_WINDOW_BIT,RECORDABLE_ANDROID,1,EGL14.EGL_NONE};
            check(EGL14.eglChooseConfig(display,attributes,0,formats,0,1,count,0)&&count[0]>0,"recordable EGL config");
            context=EGL14.eglCreateContext(display,formats[0],EGL14.EGL_NO_CONTEXT,new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION,2,EGL14.EGL_NONE},0);
            check(context!=EGL14.EGL_NO_CONTEXT,"eglCreateContext");
            window=EGL14.eglCreateWindowSurface(display,formats[0],input,new int[]{EGL14.EGL_NONE},0);
            check(window!=EGL14.EGL_NO_SURFACE&&EGL14.eglMakeCurrent(display,window,window,context),"recordable window/makeCurrent");
            String renderer=GLES20.glGetString(GLES20.GL_RENDERER);check(renderer!=null&&renderer.contains("Mali"),"not the Mali GLES renderer: "+renderer);
            System.out.println("GPU_RENDERER="+renderer);codec.start();
            drain=new Thread(()->{
                try {long until=System.nanoTime()+60000000000L;
                    while(running.get()&&!output.eos&&System.nanoTime()<until)output.pump(codec,10000);
                    output.complete();
                }catch(Throwable error){failure.set(error);}
            },"encoder-output-drain");
            drain.start();GLES20.glViewport(0,0,config.width,config.height);GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
            JSONArray frames=new JSONArray();
            for(int i=0;i<COLORS.length;i++){
                check(failure.get()==null,"output collector failed: "+failure.get());float[] rgb=COLORS[i];
                GLES20.glClearColor(rgb[0],rgb[1],rgb[2],1.f);GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                check(GLES20.glGetError()==GLES20.GL_NO_ERROR,"GLES draw error");
                long pts=EncoderCodecCheck.pts(i);
                check(EGLExt.eglPresentationTimeANDROID(display,window,pts*1000),"eglPresentationTimeANDROID");
                check(EGL14.eglSwapBuffers(display,window),"eglSwapBuffers error 0x"+Integer.toHexString(EGL14.eglGetError()));
                frames.put(new JSONObject().put("input_frame",i).put("pts_us",pts).put("expected_rgb_mean",
                    new JSONArray(new int[]{Math.round(rgb[0]*255),Math.round(rgb[1]*255),Math.round(rgb[2]*255)})));
                System.out.println("SURFACE_INPUT frame="+i+" pts="+pts);
            }
            codec.signalEndOfInputStream();drain.join(65000);check(!drain.isAlive(),"output collector timeout");
            if(failure.get()!=null)throw new AssertionError("Surface encoding failed",failure.get());
            output.save(args[0],null);
            JSONObject rgb=new JSONObject().put("codec",config.name).put("width",config.width).put("height",config.height)
                .put("renderer",renderer).put("frames",frames).put("reference","uniform GLES glClearColor RGB, rounded to 8-bit")
                .put("validation_pending","Host FFmpeg must decode all frames and compare RGB means/PTS; this tool does not claim that check");
            Files.write(Paths.get(args[0]+".rgb.json"),rgb.toString(2).getBytes(StandardCharsets.UTF_8));
            codec.stop();System.out.println("ANDROID_SURFACE_ENCODER_CONFIRMED codec="+config.name+" frames=8 Mali_GLES_inputSurface=1 original_PTS=1 EOS=1");
        } finally {
            running.set(false);if(drain!=null&&drain.isAlive())drain.join(2000);
            if(display!=EGL14.EGL_NO_DISPLAY){
                EGL14.eglMakeCurrent(display,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_CONTEXT);
                if(window!=EGL14.EGL_NO_SURFACE)EGL14.eglDestroySurface(display,window);
                if(context!=EGL14.EGL_NO_CONTEXT)EGL14.eglDestroyContext(display,context);EGL14.eglTerminate(display);
            }
            if(input!=null)input.release();codec.release();
        }
    }
}
