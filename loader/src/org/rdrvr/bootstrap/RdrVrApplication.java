package org.rdrvr.bootstrap;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.util.Log;

/** Diagnostic only: preserves stock Activity, NativeActivity and asset preparation. */
public final class RdrVrApplication extends Application {
    private static boolean loaded;
    private static native void nativeEvent(int event, String activity);
    static native String nativeProbe(String path, String verifiedSha);
    static native void nativeTraceConfigure(String path);
    static native void nativeTraceVerified(String path, String sha);
    static native String nativeTraceReport();
    static native boolean nativeProfilerEnabled();
    static native boolean nativeBenchMenuA();
    static native boolean nativeEnableShaderCapture();
    static native byte[] nativeTakeShader();
    static native boolean nativeRequestPlannerCapture();
    static native void nativeCancelPlannerCapture();
    static native int nativePlannerCaptureState();
    static native String nativePlannerCaptureReport();
    static native byte[] nativeTakePlannerCapture();
    static native void nativeCompletePlannerCapture(boolean writerExited);

    private static void event(int code, Activity activity) {
        if (loaded) nativeEvent(code, activity == null ? null : activity.getClass().getName());
    }

    @Override public void onCreate() {
        super.onCreate();
        try {
            System.loadLibrary("rdrvr");
            loaded = true;
            event(0, null);
        } catch (UnsatisfiedLinkError error) {
            if(BuildConfig.TRACE)TraceRunner.readiness.fail();
            Log.e("RdrVr", "Diagnostic library unavailable; continuing stock startup", error);
        }
        registerActivityLifecycleCallbacks(new ActivityLifecycleCallbacks() {
            public void onActivityCreated(Activity a, Bundle b) { StartupHandoff.created(a); event(1, a); }
            public void onActivityStarted(Activity a) { event(2, a); }
            public void onActivityResumed(Activity a) { StartupHandoff.resumed(a); event(3, a); }
            public void onActivityPaused(Activity a) { event(4, a); }
            public void onActivityStopped(Activity a) { event(5, a); }
            public void onActivitySaveInstanceState(Activity a, Bundle b) { event(6, a); }
            public void onActivityDestroyed(Activity a) { StartupHandoff.destroyed(a); event(7, a); }
        });
        if (loaded && BuildConfig.PROBE && getPackageName().equals(Application.getProcessName())) {
            ProbeRunner.start(this);
        }
        if (loaded && BuildConfig.TRACE && getPackageName().equals(Application.getProcessName())) {
            TraceRunner.start(this);
        }
    }
}
