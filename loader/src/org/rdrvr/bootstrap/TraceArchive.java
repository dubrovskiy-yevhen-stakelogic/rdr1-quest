package org.rdrvr.bootstrap;

import java.io.File;
import java.io.IOException;
import java.io.FileFilter;
import java.util.Arrays;
import java.util.Comparator;

/** Rotate once per process, before replacing the previous session's report. */
final class TraceArchive {
    interface Move { void replace(File source, File target) throws Exception; }
    private boolean prepared;
    private static final int RETAIN = 6;

    void beforeSave(File directory, Move move) throws Exception {
        if (prepared) return;
        File previous = new File(directory, "vulkan-trace.json");
        if (previous.isFile()) {
            File runs = new File(directory, "trace-runs");
            if (!runs.isDirectory() && !runs.mkdirs()) throw new IOException("trace archive unavailable");
            File archived = File.createTempFile("session-", ".json", runs);
            try {
                move.replace(previous, archived);
            } catch (Exception error) {
                // The old report must survive a failed rotation. Do not let
                // the caller overwrite it with a new empty startup report.
                if (archived.length() == 0) archived.delete();
                throw error;
            }
            File[] files = runs.listFiles(new FileFilter() {
                @Override public boolean accept(File file) {
                    return file.isFile() && file.getName().matches("session-[0-9]+\\.json");
                }
            });
            if (files != null) {
                Arrays.sort(files, new Comparator<File>() {
                    @Override public int compare(File a, File b) {
                        int time = Long.compare(a.lastModified(), b.lastModified());
                        return time != 0 ? time : a.getName().compareTo(b.getName());
                    }
                });
                int excess = files.length - RETAIN;
                for (File file : files) {
                    if (excess <= 0) break;
                    // Never discard the session just archived, even if the
                    // device wall clock changed or its timestamp is old.
                    if (!file.equals(archived) && file.delete()) --excess;
                }
            }
        }
        prepared = true;
    }
}
