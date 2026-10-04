/* JNI binding so an APK can call veritpath as a library instead of exec'ing a
 * binary out of its private data dir (which is fragile: noexec mounts, SELinux,
 * and the app has to ship a binary per ABI).
 *
 * The whole CLI is exposed through three natives:
 *
 *   nativeRun(String[] argv)  -> exit code, output is captured
 *   nativeLastOutput()        -> what the command printed
 *   nativeVersion()           -> "veritpath 0.2.0"
 *
 * Keeping it to "run argv, give me the text" means the Java side never has to
 * mirror C structs, and every CLI feature is available automatically.
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "vp.h"

/* the captured output of the last nativeRun(), kept until the next call */
static char *g_last_output = NULL;

static void set_last_output(char *text)
{
    free(g_last_output);
    g_last_output = text;
}

JNIEXPORT jint JNICALL
Java_dev_veritpath_Veritpath_nativeRun(JNIEnv *env, jclass cls, jobjectArray argv)
{
    (void)cls;
    if (!argv)
        return 1;

    jsize n = (*env)->GetArrayLength(env, argv);
    if (n < 0)
        return 1;

    char **c_argv = (char **)calloc((size_t)n + 1, sizeof(char *));
    if (!c_argv)
        return 1;

    jsize built = 0;
    for (jsize i = 0; i < n; i++) {
        jstring js = (jstring)(*env)->GetObjectArrayElement(env, argv, i);
        if (!js)
            continue;
        const char *chars = (*env)->GetStringUTFChars(env, js, NULL);
        if (!chars) {
            (*env)->ReleaseStringUTFChars(env, js, NULL);
            continue;
        }
        c_argv[built] = xstrdup(chars);
        (*env)->ReleaseStringUTFChars(env, js, chars);
        (*env)->DeleteLocalRef(env, js);
        if (!c_argv[built])
            break;
        built++;
    }

    int rc = 1;
    /* argv[0] is the sub-command. A leading '-' means the caller built the
     * array wrong (the image flags came first) - say so plainly instead of
     * letting the CLI report a confusing "unknown command". */
    if (built > 0 && c_argv[0] && c_argv[0][0] == '-') {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "veritpath: argv[0] must be the sub-command, got '%s'\n"
                 "put the command first, e.g. Veritpath.run(\"analyze\", "
                 "\"--boot\", path)\n", c_argv[0]);
        set_last_output(xstrdup(msg));
        goto done;
    }

    if (vp_capture_start() == 0) {
        /* stdout and stderr both land here, so a failing command still has
         * something to report */
        rc = vp_cli_run((int)built, c_argv);
        set_last_output(vp_capture_stop());
    } else {
        const char *why = vp_capture_error();
        rc = vp_cli_run((int)built, c_argv);
        /* An app cannot read logcat, so an uncaptured run looks like "no
         * output at all". Say why instead of returning an empty string. */
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "veritpath: output capture unavailable%s%s\n"
                 "pipe() failed on this platform; set a fallback directory with\n"
                 "Veritpath.setTempDir(context.getCacheDir().getAbsolutePath())\n",
                 why ? ": " : "", why ? why : "");
        set_last_output(xstrdup(msg));
    }

done:
    for (jsize i = 0; i < built; i++)
        free(c_argv[i]);
    free(c_argv);
    return (jint)rc;
}

JNIEXPORT jstring JNICALL
Java_dev_veritpath_Veritpath_nativeLastOutput(JNIEnv *env, jclass cls)
{
    (void)cls;
    if (!g_last_output)
        return (*env)->NewStringUTF(env, "");
    return (*env)->NewStringUTF(env, g_last_output);
}

JNIEXPORT jstring JNICALL
Java_dev_veritpath_Veritpath_nativeVersion(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, "veritpath " VP_VERSION);
}

JNIEXPORT void JNICALL
Java_dev_veritpath_Veritpath_nativeSetTempDir(JNIEnv *env, jclass cls, jstring dir)
{
    (void)cls;
    if (!dir) {
        vp_capture_set_dir(NULL);
        return;
    }
    const char *chars = (*env)->GetStringUTFChars(env, dir, NULL);
    if (!chars)
        return;
    vp_capture_set_dir(chars);
    (*env)->ReleaseStringUTFChars(env, dir, chars);
}

JNIEXPORT void JNICALL
Java_dev_veritpath_Veritpath_nativeFree(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    set_last_output(NULL);
}
