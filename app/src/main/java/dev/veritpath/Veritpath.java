package dev.veritpath;

/**
 * Java wrapper around the veritpath C core.
 *
 * <p>Load the native library once (usually in {@code Application.onCreate} or a
 * static initialiser), then call the helpers. Everything the CLI can do is
 * available, because the binding simply runs the command and hands back what it
 * printed.
 *
 * <pre>{@code
 * Veritpath.Result r = Veritpath.run("analyze", "--brief", "--boot", boot.getAbsolutePath());
 * if (r.exitCode == 0) {
 *     String arch = r.line("ARCH");
 * }
 * }</pre>
 *
 * <p>The calls are synchronous and do no I/O beyond what the command itself
 * does, so run them off the main thread — a 192MB GKI image takes a moment.
 */
public final class Veritpath {

    /** What a command returned. */
    public static final class Result {
        /** Process-style exit code: 0 on success. */
        public final int exitCode;
        /** Everything the command printed. */
        public final String output;

        Result(int exitCode, String output) {
            this.exitCode = exitCode;
            this.output = output;
        }

        /** Value of a {@code KEY:VALUE} line, or null when absent. */
        public String line(String key) {
            if (output == null) return null;
            String prefix = key + ":";
            for (String l : output.split("\n")) {
                if (l.startsWith(prefix)) return l.substring(prefix.length()).trim();
            }
            return null;
        }

        public boolean ok() {
            return exitCode == 0;
        }
    }

    private static boolean loaded;

    private Veritpath() {}

    /** Loads {@code libveritpath.so}. Safe to call more than once. */
    public static synchronized void load() {
        if (loaded) return;
        System.loadLibrary("veritpath");
        loaded = true;
    }

    /**
     * Runs a command. The arguments are exactly what you would type after
     * {@code veritpath} on a shell, one String per token.
     */
    public static Result run(String... args) {
        load();
        int code = nativeRun(args);
        return new Result(code, nativeLastOutput());
    }

    /** Analyse images; returns the {@code --brief} KEY:VALUE report. */
    public static Result analyze(String... args) {
        String[] full = new String[args.length + 1];
        full[0] = "analyze";
        System.arraycopy(args, 0, full, 1, args.length);
        return run(full);
    }

    /** Analyse and get structured JSON instead of the text report. */
    public static String analyzeJson(String... args) {
        String[] full = new String[args.length + 2];
        full[0] = "analyze";
        full[1] = "--json";
        System.arraycopy(args, 0, full, 2, args.length);
        return run(full).output;
    }

    /** Inject a payload. Returns exit code 0 on success. */
    public static Result inject(String payloadDir, String outputPath, String... images) {
        String[] full = new String[images.length + 5];
        full[0] = "inject";
        System.arraycopy(images, 0, full, 1, images.length);
        full[images.length + 1] = "-p";
        full[images.length + 2] = payloadDir;
        full[images.length + 3] = "-o";
        full[images.length + 4] = outputPath;
        return run(full);
    }

    /**
     * Check a patched image. {@link Result#ok()} is false when something is
     * missing, so this can gate a flash.
     */
    public static Result verify(String image) {
        return run("verify", image);
    }

    /** Validate a payload directory without touching an image. */
    public static Result payloadCheck(String dir) {
        return run("payload-check", dir);
    }

    /** Header diagnostics — start here when an image will not parse. */
    public static Result hexdump(String image) {
        return run("hexdump", image);
    }

    /** Version of the native core, e.g. {@code "veritpath 0.2.0"}. */
    public static String version() {
        load();
        return nativeVersion();
    }

    /** Release the buffer holding the last command's output. */
    public static void release() {
        if (loaded) nativeFree();
    }

    private static native int nativeRun(String[] argv);

    private static native String nativeLastOutput();

    private static native String nativeVersion();

    private static native void nativeFree();
}
