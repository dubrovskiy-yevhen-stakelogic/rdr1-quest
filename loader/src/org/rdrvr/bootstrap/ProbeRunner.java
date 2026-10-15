package org.rdrvr.bootstrap;

import android.app.Application;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/** One bounded background diagnostic. Does not preload or call the game. */
final class ProbeRunner implements Runnable {
    private final Application app;
    private ProbeRunner(Application app) { this.app = app; }
    static void start(Application app) {
        Thread thread = new Thread(new ProbeRunner(app), "RdrVrObserver");
        thread.setDaemon(true);
        thread.start();
    }
    private void save(JSONObject report) throws Exception {
        File base = app.getExternalFilesDir(null);
        if (base == null) base = app.getFilesDir();
        File folder = new File(base, "rdrvr");
        if (!folder.isDirectory() && !folder.mkdirs()) throw new Exception("report directory unavailable");
        File temp = new File(folder, "engine-probe.tmp");
        File target = new File(folder, "engine-probe.json");
        try (FileOutputStream file = new FileOutputStream(temp)) {
            file.write(report.toString(2).getBytes(StandardCharsets.UTF_8));
            file.getFD().sync();
        }
        // Same-directory POSIX rename replaces the previous diagnostic atomically.
        android.system.Os.rename(temp.getAbsolutePath(), target.getAbsolutePath());
    }
    @Override public void run() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
        JSONObject report = new JSONObject();
        long start = SystemClock.elapsedRealtime();
        try {
            report.put("schemaVersion", 1).put("buildId", BuildConfig.BUILD_ID)
                .put("mode", "read-only-observation").put("hooksEnabled", false)
                .put("callTracing", false).put("gameThreadIds", JSONObject.NULL)
                .put("earlyVulkanCreationCaptured", false).put("runtimeGameplay", "PENDING");
            File engine = new File(app.getApplicationInfo().nativeLibraryDir, "librdr.so").getCanonicalFile();
            long bytes = engine.length(), modified = engine.lastModified();
            report.put("engineBytes", bytes).put("status", "HASHING");
            save(report);
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[1024 * 1024];
            try (FileInputStream file = new FileInputStream(engine)) {
                for (int n; (n = file.read(buffer)) != -1;) sha.update(buffer, 0, n);
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : sha.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
            report.put("engineSha256", hex.toString()).put("hashElapsedMs", SystemClock.elapsedRealtime() - start);
            if (bytes != engine.length() || modified != engine.lastModified()) throw new Exception("engine changed during hashing");
            if (!BuildConfig.ENGINE_SHA.equals(hex.toString())) {
                report.put("status", "HASH_REJECTED"); save(report);
                Log.e("RdrVrProbe", "engine identity rejected; no symbol inspection"); return;
            }
            JSONArray transitions = new JSONArray();
            report.put("transitions", transitions).put("status", "OBSERVING");
            String previous = "";
            long totalUs = 0, maxUs = 0;
            int samples = 0;
            int transitionsDropped = 0;
            for (int i = 0; i < 120; i++) {
                if (engine.length() != bytes || engine.lastModified() != modified) throw new Exception("engine changed after verification");
                JSONObject sample = new JSONObject(RdrVrApplication.nativeProbe(engine.getPath(), hex.toString()));
                samples++;
                long us = sample.optLong("snapshotUs"); totalUs += us; maxUs = Math.max(maxUs, us);
                String state = sample.optString("status") + ":" + sample.optInt("symbolsMatched") + ":" + sample.optInt("initializedSlots");
                // Include individual symbol/target changes, not just aggregate counts.
                String signature = state + sample.optString("base") + sample.optJSONArray("symbols");
                report.put("latestSnapshot", sample);
                if (!signature.equals(previous)) {
                    sample.put("elapsedMs", SystemClock.elapsedRealtime() - start);
                    previous = signature;
                    if (transitions.length() < 32) {
                        transitions.put(sample);
                        Log.i("RdrVrProbe", "build=" + BuildConfig.BUILD_ID + " state=" + state + " observerTid=" + sample.optInt("observerTid") + " hooks=false calls=false");
                        report.put("samples", samples).put("snapshotTotalUs", totalUs).put("snapshotMaxUs", maxUs);
                        save(report);
                    } else transitionsDropped++;
                }
                Thread.sleep(1000);
            }
            report.put("status", "COMPLETE").put("samples", samples).put("snapshotTotalUs", totalUs)
                .put("snapshotMaxUs", maxUs).put("elapsedMs", SystemClock.elapsedRealtime() - start)
                .put("transitionsDropped", transitionsDropped);
            save(report);
            Log.i("RdrVrProbe", "bounded observation complete; report=files/rdrvr/engine-probe.json; runtime acceptance pending");
        } catch (Exception error) {
            Log.e("RdrVrProbe", "observation stopped; game continues", error);
            try { report.put("status", "FAILED").put("error", error.toString()); save(report); } catch (Exception ignored) { }
        }
    }
}
