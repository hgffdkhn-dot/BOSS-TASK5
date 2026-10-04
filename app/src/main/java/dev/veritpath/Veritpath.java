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

    /**
     * An input image and the flag that names it. Use the factories so the flag
     * can never be wrong or missing:
     *
     * <pre>{@code
     * Veritpath.inject(Veritpath.Image.initBoot(path), payloadDir, outDir);
     * }</pre>
     */
    public static final class Image {
        /** null means "no flag" - the CLI works the role out from the content. */
        final String flag;
        final String path;

        private Image(String flag, String path) {
            if (path == null) throw new IllegalArgumentException("path is required");
            this.flag = flag;
            this.path = path;
        }

        public static Image boot(String path)       { return new Image("--boot", path); }
        public static Image initBoot(String path)   { return new Image("--init-boot", path); }
        public static Image vendorBoot(String path) { return new Image("--vendor-boot", path); }
        public static Image recovery(String path)   { return new Image("--recovery", path); }

        /** Let the CLI detect whether this is a boot, init_boot or vendor_boot. */
        public static Image auto(String path)       { return new Image(null, path); }
    }

    private static boolean loaded;

    private Veritpath() {}

    /**
     * Optional. Sets a fallback directory for output capture on platforms
     * without pipes.
     *
     * <p><b>You do not need this on Android or Linux.</b> Capture is a pipe
     * drained into memory, so it needs no writable directory at all. This is
     * only a last resort for exotic platforms where {@code pipe()} is
     * unavailable.
     *
     * @param dir a directory the app can write to, e.g. {@code getCacheDir()}
     */
    public static void setTempDir(String dir) {
        load();
        nativeSetTempDir(dir);
    }

    /** Loads {@code libveritpath.so}. Safe to call more than once. */
    public static synchronized void load() {
        if (loaded) return;
        System.loadLibrary("veritpath");
        loaded = true;
    }

    /**
     * Runs a command. The arguments are exactly what you would type after
     * {@code veritpath} on a shell, one String per token, <b>so the
     * sub-command has to be {@code args[0]}</b>.
     *
     * <pre>{@code
     * Veritpath.run("analyze", "--brief", "--boot", bootPath);   // correct
     * Veritpath.run("--boot", bootPath, "analyze");              // rejected
     * }</pre>
     */
    public static Result run(String... args) {
        load();
        if (args != null && args.length > 0 && args[0] != null && args[0].startsWith("-")) {
            throw new IllegalArgumentException(
                "the sub-command must be args[0], got '" + args[0] + "' - "
                + "write Veritpath.run("analyze", "--boot", path)");
        }
        int code = nativeRun(args);
        return new Result(code, nativeLastOutput());
    }

    /**
     * Analyse images. Pass one or more {@link Image}s; the correct flags are
     * emitted for you.
     */
    public static Result analyze(Image... images) {
        if (images == null || images.length == 0)
            throw new IllegalArgumentException("at least one image is required");
        String[] head = {"analyze", "--brief"};
        return run(concat(head, withFlags(images), null));
    }

    /** Analyse and get structured JSON instead of the text report. */
    public static String analyzeJson(Image... images) {
        if (images == null || images.length == 0)
            throw new IllegalArgumentException("at least one image is required");
        String[] head = {"analyze", "--json"};
        return run(concat(head, withFlags(images), null)).output;
    }

    /**
     * Analyse with full control over the flags, e.g.
     * {@code analyzeRaw("--boot", bootPath, "--init-boot", initBootPath, "-v")}.
     */
    public static Result analyzeRaw(String... args) {
        String[] full = new String[args.length + 1];
        full[0] = "analyze";
        System.arraycopy(args, 0, full, 1, args.length);
        return run(full);
    }

    /**
     * Inject a payload into an image.
     *
     * <p>The image is supplied as an {@link Image}, so the correct flag is
     * always emitted. The old signature took raw strings for the image, which
     * made it easy to pass a bare path that the CLI then ignored because it
     * only looks at {@code --boot} / {@code --init-boot} / {@code --vendor-boot}
     * / {@code --recovery}.
     */
    public static Result inject(Image image, String payloadDir, String outputPath,
                               String... extraArgs) {
        if (image == null) throw new IllegalArgumentException("image is required");
        if (payloadDir == null) throw new IllegalArgumentException("payloadDir is required");
        if (outputPath == null) throw new IllegalArgumentException("outputPath is required");
        String[] head = {"inject", "-p", payloadDir, "-o", outputPath};
        return run(concat(head, withFlags(image), extraArgs));
    }

    /** Inject into several images at once (e.g. boot + init_boot). */
    public static Result inject(Image[] images, String payloadDir, String outputPath,
                               String... extraArgs) {
        if (images == null || images.length == 0)
            throw new IllegalArgumentException("at least one image is required");
        if (payloadDir == null) throw new IllegalArgumentException("payloadDir is required");
        if (outputPath == null) throw new IllegalArgumentException("outputPath is required");
        String[] head = {"inject", "-p", payloadDir, "-o", outputPath};
        return run(concat(head, withFlags(images), extraArgs));
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

    /** Turns one or more Images into their CLI flag tokens. */
    private static String[] withFlags(Image... images) {
        int n = 0;
        for (Image i : images) n += (i.flag == null ? 1 : 2);
        String[] out = new String[n];
        int at = 0;
        for (Image i : images) {
            if (i.flag != null) out[at++] = i.flag;
            out[at++] = i.path;
        }
        return out;
    }

    /**
     * Concatenates argument arrays.
     *
     * <p><b>Order is the whole contract here:</b> {@code vp_cli_run} takes
     * {@code argv[0]} as the sub-command, so the sub-command must come first.
     * An earlier revision passed the image flags first and produced
     * {@code ["--init-boot", path, "inject", ...]}, which the CLI rejected with
     * {@code unknown command: --init-boot} and exit 1. Every call site must
     * pass the command head as {@code first}.
     */
    private static String[] concat(String[] first, String[] second, String[] third) {
        int n = first.length + second.length + (third == null ? 0 : third.length);
        String[] out = new String[n];
        System.arraycopy(first, 0, out, 0, first.length);
        System.arraycopy(second, 0, out, first.length, second.length);
        if (third != null)
            System.arraycopy(third, 0, out, first.length + second.length, third.length);
        return out;
    }

    private static native int nativeRun(String[] argv);

    private static native String nativeLastOutput();

    private static native String nativeVersion();

    private static native void nativeSetTempDir(String dir);

    private static native void nativeFree();
}
