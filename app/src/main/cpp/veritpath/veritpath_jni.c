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
    if (vp_capture_start() == 0) {
        rc = vp_cli_run((int)built, c_argv);
        set_last_output(vp_capture_stop());
    } else {
        rc = vp_cli_run((int)built, c_argv);
        set_last_output(NULL);
    }

    /* also surface stderr-ish errors: vp_err writes to stderr, which the app
     * can see in logcat, so nothing extra is needed here */

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
Java_dev_veritpath_Veritpath_nativeFree(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    set_last_output(NULL);
}
