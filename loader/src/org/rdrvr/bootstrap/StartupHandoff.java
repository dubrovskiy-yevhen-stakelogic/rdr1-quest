package org.rdrvr.bootstrap;

import android.app.Activity;
import android.view.View;
import android.view.ViewTreeObserver;
import java.lang.ref.WeakReference;
import java.util.ArrayList;

/** Keep the startup task alive until the native window actually receives focus. */
final class StartupHandoff {
    private static final String GAME="com.rockstargames.rdr.NativeGameActivity";
    private static final ArrayList<WeakReference<Activity>> pending=new ArrayList<>();
    private static WeakReference<Activity> watching=new WeakReference<>(null);
    private static ViewTreeObserver.OnWindowFocusChangeListener listener;
    static void created(Activity a) {
        String name=a.getClass().getName();
        if(a instanceof PreflightActivity || (name.equals(BuildConfig.LAUNCHER) && !name.equals(GAME)))
            pending.add(new WeakReference<>(a));
    }
    private static void finishStartup() {
        for(WeakReference<Activity> ref:pending){
            Activity a=ref.get();
            if(a!=null&&!a.isFinishing()&&!a.isDestroyed())a.finish();
        }
        pending.clear();
    }
    private static void detach() {
        Activity a=watching.get();
        if(a!=null&&listener!=null){
            ViewTreeObserver tree=a.getWindow().getDecorView().getViewTreeObserver();
            if(tree.isAlive())tree.removeOnWindowFocusChangeListener(listener);
        }
        watching.clear();listener=null;
    }
    static void resumed(final Activity a) {
        if(!GAME.equals(a.getClass().getName()))return;
        detach();
        final View view=a.getWindow().getDecorView();
        watching=new WeakReference<>(a);
        listener=new ViewTreeObserver.OnWindowFocusChangeListener(){
            @Override public void onWindowFocusChanged(boolean focused){
                if(focused){finishStartup();detach();}
            }
        };
        view.getViewTreeObserver().addOnWindowFocusChangeListener(listener);
        // onResume can also run after an already-focused window is restored.
        if(view.hasWindowFocus()){finishStartup();detach();}
    }
    static void destroyed(Activity a) {
        if(GAME.equals(a.getClass().getName())){detach();finishStartup();}
        for(int i=pending.size()-1;i>=0;--i)
            if(pending.get(i).get()==null||pending.get(i).get()==a)pending.remove(i);
    }
}
