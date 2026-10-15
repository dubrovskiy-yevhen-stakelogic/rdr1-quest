package org.rdrvr.bootstrap;

import android.app.Application;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/** Bounded diagnostic; only NativeActivity's normal entry may arm interposition. */
final class TraceRunner implements Runnable {
    static final StartupGate.Readiness readiness=new StartupGate.Readiness();
    private final long processStartedMs=System.currentTimeMillis();
    private final Application app;
    private final File engine;
    private final TraceArchive archive = new TraceArchive();
    private TraceRunner(Application app, File engine) {this.app=app;this.engine=engine;}
    static void start(Application app) {
        try {
            File engine=new File(app.getApplicationInfo().nativeLibraryDir,"librdr.so").getCanonicalFile();
            RdrVrApplication.nativeTraceConfigure(engine.getPath());
            Thread worker=new Thread(new TraceRunner(app,engine),"RdrVrTraceWriter");
            worker.setDaemon(true);worker.start();
        } catch(Exception | LinkageError error) {readiness.fail();Log.e("RdrVrTrace","trace preparation unavailable",error);}
    }
    private void save(JSONObject data) throws Exception {
        File base=app.getExternalFilesDir(null);if(base==null)base=app.getFilesDir();
        File dir=new File(base,"rdrvr");if(!dir.isDirectory()&&!dir.mkdirs())throw new Exception("report directory unavailable");
        archive.beforeSave(dir, new TraceArchive.Move() {
            @Override public void replace(File source, File destination) throws Exception {
                android.system.Os.rename(source.getPath(),destination.getPath());
            }
        });
        File temp=new File(dir,"vulkan-trace.tmp"),target=new File(dir,"vulkan-trace.json");
        try(FileOutputStream out=new FileOutputStream(temp)) {out.write(data.toString(2).getBytes(StandardCharsets.UTF_8));out.getFD().sync();}
        android.system.Os.rename(temp.getPath(),target.getPath());
    }
    private File shaderDirectory;
    private int shadersWritten;
    private String shaderError="";
    private void configureShaderCapture() {
        if(!BuildConfig.BENCH_CONTROL)return;
        File base=app.getExternalFilesDir(null);if(base==null)return;
        File dir=new File(base,"rdrvr"),command=new File(dir,"bench-capture-shaders");
        if(!command.isFile())return;
        try {
            long age=System.currentTimeMillis()-command.lastModified();
            if(command.length()!=7 || age<0 || age>900000){command.delete();return;}
            byte[] bytes=new byte[8];int count;
            try(FileInputStream input=new FileInputStream(command)){count=input.read(bytes);}
            if(!command.delete() || count!=7 || !new String(bytes,0,count,StandardCharsets.US_ASCII).equals("CAPTURE"))return;
            File target=new File(dir,"shader-capture-"+processStartedMs);
            if(!target.mkdir())throw new Exception("capture directory unavailable");
            if(!RdrVrApplication.nativeEnableShaderCapture())throw new Exception("capture allocation rejected");
            shaderDirectory=target;
        }catch(Exception | LinkageError | OutOfMemoryError error){shaderError=error.toString();Log.w("RdrVrBench","Shader capture unavailable",error);}
    }
    private void drainShaders() {
        if(shaderDirectory==null || !shaderError.isEmpty())return;
        try {
            for(int i=0;i<32;++i) {
                byte[] bytes=RdrVrApplication.nativeTakeShader();if(bytes==null)break;
                if(bytes.length<20 || bytes.length>512*1024)throw new Exception("invalid capture size");
                byte[] hash=MessageDigest.getInstance("SHA-256").digest(bytes);
                StringBuilder name=new StringBuilder();for(byte b:hash)name.append(String.format(java.util.Locale.ROOT,"%02x",b&255));
                File temp=new File(shaderDirectory,"module.tmp"),target=new File(shaderDirectory,name+".spv");
                try(FileOutputStream output=new FileOutputStream(temp)){output.write(bytes);}
                android.system.Os.rename(temp.getPath(),target.getPath());++shadersWritten;
            }
        }catch(Exception | LinkageError | OutOfMemoryError error){shaderError=error.toString();Log.w("RdrVrBench","Shader capture writer stopped",error);}
    }
    private boolean plannerAttempted,plannerPending;
    private long plannerRequestedMs;
    private String plannerFile="",plannerError="";
    private void pollPlannerCapture() {
        if(!BuildConfig.BENCH_CONTROL || plannerAttempted)return;
        File base=app.getExternalFilesDir(null);if(base==null)return;
        File command=new File(new File(base,"rdrvr"),"bench-capture-occlusion");
        if(!command.isFile())return;
        try {
            long age=System.currentTimeMillis()-command.lastModified();
            if(command.length()!=7 || age<0 || age>900000){command.delete();return;}
            byte[] bytes=new byte[8];int count;
            try(FileInputStream input=new FileInputStream(command)){count=input.read(bytes);}
            // Delete FIRST. Process restart or a failed claim cannot replay the
            // same explicit command, and no setting change synthesizes one.
            if(!command.delete())return;
            plannerAttempted=true;
            if(count!=7 || !new String(bytes,0,count,StandardCharsets.US_ASCII).equals("CAPTURE"))return;
            plannerPending=RdrVrApplication.nativeRequestPlannerCapture();
            if(!plannerPending)throw new Exception("one-shot capture request rejected");
            plannerRequestedMs=SystemClock.elapsedRealtime();
        } catch(Exception | LinkageError | OutOfMemoryError error){plannerError=error.toString();Log.w("RdrVrBench","Planner capture request failed",error);}
    }
    private void drainPlannerCapture() {
        if(!plannerPending)return;
        boolean writing=false;
        try {
            int state=RdrVrApplication.nativePlannerCaptureState();
            if(state==2 && SystemClock.elapsedRealtime()-plannerRequestedMs>300000){
                RdrVrApplication.nativeCancelPlannerCapture();state=RdrVrApplication.nativePlannerCaptureState();
            }
            if(state!=4 && state!=6)return; // Ready or Failed; claim is never interrupted.
            plannerPending=false;writing=true;
            File base=app.getExternalFilesDir(null);if(base==null)base=app.getFilesDir();
            File dir=new File(base,"rdrvr");if(!dir.isDirectory()&&!dir.mkdirs())throw new Exception("capture directory unavailable");
            String name="occlusion-scene-"+processStartedMs;
            JSONObject info=new JSONObject(RdrVrApplication.nativePlannerCaptureReport());
            info.put("buildId",BuildConfig.BUILD_ID).put("captureRequestExplicit",true).put("writerThread",Thread.currentThread().getName());
            if(state==4){
                byte[] bytes=RdrVrApplication.nativeTakePlannerCapture();
                if(bytes==null||bytes.length<8||bytes.length>32*1024*1024)throw new Exception("invalid capture payload");
                String magic=new String(bytes,0,8,StandardCharsets.US_ASCII);if(!magic.equals("RDROCC01"))throw new Exception("invalid capture magic");
                byte[] digest=MessageDigest.getInstance("SHA-256").digest(bytes);
                StringBuilder sha=new StringBuilder();for(byte b:digest)sha.append(String.format(java.util.Locale.ROOT,"%02x",b&255));
                File tmp=new File(dir,name+".rdrocc.tmp"),target=new File(dir,name+".rdrocc");
                try(FileOutputStream out=new FileOutputStream(tmp)){out.write(bytes);out.getFD().sync();}
                android.system.Os.rename(tmp.getPath(),target.getPath());plannerFile=target.getName();
                info.put("file",plannerFile).put("bytes",bytes.length).put("sha256",sha.toString());
            }else plannerError="native capture failed: "+info;
            File temp=new File(dir,name+".json.tmp"),target=new File(dir,name+".json");
            try(FileOutputStream out=new FileOutputStream(temp)){out.write(info.toString(2).getBytes(StandardCharsets.UTF_8));out.getFD().sync();}
            android.system.Os.rename(temp.getPath(),target.getPath());
        }catch(Exception | LinkageError | OutOfMemoryError error){
            plannerPending=false;
            // Also close failures BEFORE writing=true, including a failed state
            // query. Cancel Armed or arrange Claimed cleanup at Scene finish;
            // otherwise the reporter would stop draining a live native request.
            // Cleanup precedes error formatting, which may itself allocate.
            try {RdrVrApplication.nativeCompletePlannerCapture(true);}
            catch(Exception | LinkageError | OutOfMemoryError ignored) { }
            plannerError=error.toString();Log.w("RdrVrBench","Planner capture writer stopped",error);
        }
        finally {if(writing)RdrVrApplication.nativeCompletePlannerCapture(false);}
    }
    private void pollBenchCommand() {
        if(!BuildConfig.BENCH_CONTROL)return;
        File base=app.getExternalFilesDir(null);if(base==null)return;
        File command=new File(new File(base,"rdrvr"),"bench-menu-a");
        if(!command.isFile())return;
        try {
            long modified=command.lastModified(),now=System.currentTimeMillis();
            if(command.length()>16||modified<processStartedMs||modified>now+2000||now-modified>15000){command.delete();return;}
            byte[] bytes=new byte[16];int count;
            try(FileInputStream input=new FileInputStream(command)){count=input.read(bytes);}
            if(!command.delete())return;
            if(count>0&&new String(bytes,0,count,StandardCharsets.US_ASCII).trim().equals("A"))
                Log.i("RdrVrBench","Menu A queued="+RdrVrApplication.nativeBenchMenuA());
        }catch(Exception error){Log.w("RdrVrBench","Command rejected",error);}
    }
    // Explicit one-shot diagnostics; does not change profiler mode or enable clocks.
    static String consumeSnapshotRequest(File command,long processStart,long now) throws Exception {
        if(!command.isFile())return "";
        long modified=command.lastModified();
        if(command.length()!=32 || modified<processStart || modified>now+2000 || now-modified>15000){command.delete();return "";}
        byte[] bytes=new byte[33];int count;
        try(FileInputStream input=new FileInputStream(command)){count=input.read(bytes);}
        if(!command.delete() || count!=32)return "";
        String token=new String(bytes,0,count,StandardCharsets.US_ASCII);
        return token.matches("[a-f0-9]{32}")?token:"";
    }
    private String pollSnapshotRequest() {
        if(!BuildConfig.BENCH_CONTROL)return "";
        File base=app.getExternalFilesDir(null);if(base==null)return "";
        try {return consumeSnapshotRequest(new File(new File(base,"rdrvr"),"bench-trace-snapshot"),processStartedMs,System.currentTimeMillis());}
        catch(Exception error){Log.w("RdrVrBench","Snapshot request rejected",error);return "";}
    }
    private void pollSelfMapsRequest() {
        if(!BuildConfig.BENCH_CONTROL)return;
        File base=app.getExternalFilesDir(null);if(base==null)return;
        File dir=new File(base,"rdrvr");
        String token;
        try {token=consumeSnapshotRequest(new File(dir,"bench-selfmaps-snapshot"),processStartedMs,System.currentTimeMillis());}
        catch(Exception error){Log.w("RdrVrBench","Self maps request rejected",error);return;}
        if(token.isEmpty())return;
        File temp=new File(dir,"selfmaps-snapshot.tmp"),target=new File(dir,"selfmaps-snapshot.json");
        try {
            long before=SystemClock.elapsedRealtime();
            JSONObject data=new JSONObject().put("schema",1).put("pid",Process.myPid())
                .put("nativeBuildId",BuildConfig.BUILD_ID).put("nonce",token)
                .put("boottimeBeforeMs",before);
            try {
                String maps=SelfMapsSnapshot.readSelf();
                long after=SystemClock.elapsedRealtime();
                if(after<before || after-before>1000)throw new Exception("maps read exceeded time bound");
                data.put("boottimeAfterMs",after).put("rawMaps",maps).put("status","SELF_MAPS_COMPLETE");
            }catch(Exception | OutOfMemoryError error) {
                data.put("boottimeAfterMs",SystemClock.elapsedRealtime())
                    .put("status","SELF_MAPS_REJECTED").put("error",error.getClass().getName());
                data.remove("rawMaps");
            }
            try(FileOutputStream out=new FileOutputStream(temp)) {
                out.write(data.toString().getBytes(StandardCharsets.UTF_8));out.getFD().sync();
            }
            android.system.Os.rename(temp.getPath(),target.getPath());
        }catch(Exception | OutOfMemoryError error) {
            temp.delete();Log.w("RdrVrBench","Self maps report unavailable",error);
        }
    }
    @Override public void run() {
        final long start=SystemClock.elapsedRealtime();
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
            configureShaderCapture();
            long length=engine.length(),modified=engine.lastModified();
            MessageDigest sha=MessageDigest.getInstance("SHA-256");byte[] bytes=new byte[1024*1024];
            try(FileInputStream file=new FileInputStream(engine)) {for(int n;(n=file.read(bytes))!=-1;)sha.update(bytes,0,n);}
            StringBuilder hash=new StringBuilder();for(byte b:sha.digest())hash.append(String.format(java.util.Locale.ROOT,"%02x",b&255));
            long hashMs=SystemClock.elapsedRealtime()-start;
            if(length!=engine.length() || modified!=engine.lastModified())throw new Exception("engine changed while hashing");
            if(!BuildConfig.ENGINE_SHA.equals(hash.toString()))throw new Exception("engine SHA rejected");
            RdrVrApplication.nativeTraceVerified(engine.getPath(),hash.toString());
            if(!new JSONObject(RdrVrApplication.nativeTraceReport()).optBoolean("hashVerified"))
                throw new Exception("native verification rejected");
            readiness.ready();
            String previous="";long lastSaved=-1;boolean profilingWasEnabled=true;
            // Asset preparation can outlive the capture window. Start that window
            // at the native entry, with a separate bounded wait for the entry.
            final long waitingSince=SystemClock.elapsedRealtime();long entrySince=-1;
            for(;;) {
                pollBenchCommand();
                drainShaders();
                pollPlannerCapture();
                drainPlannerCapture();
                pollSelfMapsRequest();
                String snapshotToken=pollSnapshotRequest();
                boolean profiling=RdrVrApplication.nativeProfilerEnabled();
                if(entrySince>=0 && !profiling && !profilingWasEnabled && snapshotToken.isEmpty()) {
                    Thread.sleep(1000);continue;
                }
                boolean profilingChanged=profiling!=profilingWasEnabled;profilingWasEnabled=profiling;
                String raw=RdrVrApplication.nativeTraceReport();
                JSONObject data=new JSONObject(raw);
                data.put("profilerEnabled",profiling);
                data.put("reportPid",Process.myPid()).put("reportElapsedRealtimeMs",SystemClock.elapsedRealtime());
                if(!snapshotToken.isEmpty())data.put("snapshotToken",snapshotToken);
                if(BuildConfig.BENCH_CONTROL && plannerAttempted)data.put("plannerCaptureWriter",new JSONObject()
                    .put("pending",plannerPending).put("file",plannerFile).put("error",plannerError)
                    .put("native",new JSONObject(RdrVrApplication.nativePlannerCaptureReport())));
                if(BuildConfig.BENCH_CONTROL)data.put("shaderCaptureWriter",new JSONObject()
                    .put("directory",shaderDirectory==null?"":shaderDirectory.getName())
                    .put("written",shadersWritten).put("error",shaderError));
                long now=SystemClock.elapsedRealtime();
                if(entrySince<0 && data.optBoolean("entrySeen"))entrySince=now;
                boolean expired=entrySince<0 && now-waitingSince>=600000;
                boolean complete=profilingChanged && !profiling;
                String signature=data.optString("status")+data.optJSONArray("events")
                    +data.optBoolean("creationBudgetExceeded")+data.optBoolean("submitSamplesLimited")
                    +data.optBoolean("lifetimeBudgetExceeded")
                    +data.optBoolean("patchFault")+data.optInt("recordFailures")+data.optInt("eventsDropped");
                // Keep counters fresh after the bounded event sample fills.
                // DETAIL records bounded snapshots until switched off; FPS/OFF sleeps
                // without constructing reports. Later DETAIL activation remains available.
                if(!snapshotToken.isEmpty() || profilingChanged || !signature.equals(previous) || lastSaved<0 || now-lastSaved>=10000 || complete || expired) {
                    data.put("hashElapsedMs",hashMs)
                        .put("writerElapsedMs",now-start).put("writerComplete",complete)
                        .put("entryWaitExpired",expired).put("preflightVerified",true);
                    save(data);previous=signature;lastSaved=now;
                }
                if(expired)break;
                Thread.sleep(1000);
            }
        } catch(Exception | LinkageError error) {
            readiness.fail();
            Log.e("RdrVrTrace","trace writer stopped",error);
            try {JSONObject report=new JSONObject(RdrVrApplication.nativeTraceReport());
                report.put("writerError",error.toString()).put("writerComplete",false);save(report);
            } catch(Exception ignored) { }
        } finally {
            if(BuildConfig.BENCH_CONTROL && plannerAttempted)RdrVrApplication.nativeCompletePlannerCapture(true);
        }
    }
}
