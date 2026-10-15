package org.rdrvr.bootstrap;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

/** Launcher-only preparation. Original asset preparation and game Activity remain intact. */
public final class PreflightActivity extends Activity {
    private final Handler handler=new Handler(Looper.getMainLooper());
    private StartupGate gate;
    private TextView message;
    private ProgressBar progress;
    private final Runnable poll=new Runnable() {
        @Override public void run() {
            if(isFinishing() || isDestroyed())return;
            switch(gate.next(TraceRunner.readiness.get())) {
                case WAIT: handler.postDelayed(this,100);break;
                case ERROR: showError();break;
                case LAUNCH:
                    try {
                        if (!RdrVrApplication.prepareGameMode(PreflightActivity.this)) {
                            showError();
                            break;
                        }
                        Intent original=new Intent(getIntent());
                        original.setComponent(new android.content.ComponentName(getPackageName(),
                            BuildConfig.LAUNCHER));
                        original.setFlags(original.getFlags() & ~(Intent.FLAG_ACTIVITY_NEW_TASK |
                            Intent.FLAG_ACTIVITY_MULTIPLE_TASK | Intent.FLAG_ACTIVITY_NEW_DOCUMENT |
                            Intent.FLAG_ACTIVITY_CLEAR_TASK));
                        startActivity(original);
                    } catch(RuntimeException error) {
                        Log.e("RdrVrTrace","original launcher unavailable",error);showError();
                    }
                    break;
                default: break;
            }
        }
    };
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        gate=new StartupGate(saved!=null && saved.getBoolean("rdrvr.launched",false));
        LinearLayout layout=new LinearLayout(this);layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);layout.setPadding(32,32,32,32);
        progress=new ProgressBar(this);layout.addView(progress);
        message=new TextView(this);message.setText("Preparing the game...");message.setTextSize(22);
        message.setGravity(Gravity.CENTER);layout.addView(message);setContentView(layout);
        if(!BuildConfig.TRACE) {showError();finish();}
    }
    private void showError() {
        progress.setVisibility(android.view.View.GONE);
        message.setText("The game could not be prepared. Close the app and start it again.");
    }
    @Override public void onResume() {super.onResume();gate.resume();handler.post(poll);}
    @Override public void onPause() {gate.pause();handler.removeCallbacks(poll);super.onPause();}
    @Override public void onDestroy() {handler.removeCallbacks(poll);super.onDestroy();}
    @Override public void onSaveInstanceState(Bundle saved) {
        saved.putBoolean("rdrvr.launched",gate.launched());super.onSaveInstanceState(saved);
    }
}
