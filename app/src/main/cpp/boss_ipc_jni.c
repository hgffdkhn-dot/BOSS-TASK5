/* BOSS · JNI 胶水层（任务6）
 *
 * 这里只做三件事：把 Java 参数搬进 C、调 boss_ipc_run、把结果搬回 Java。
 * 不做任何授权/提权/路径决策——那些都在 daemon 侧（架构红线第 1 条）。
 *
 * 返回值约定（Kotlin 侧按这个解析）：
 *   "DOWN\n"                       连不上 daemon（没装 / 没起来 / 劫持没成）
 *   "CODE=<n> EXIT=<n> TMO=<0|1>\n<输出…>"
 *
 * 为什么把两个码放在报文头里：JNI 返回一个 String 最省事，而"响应码"和
 * "退出码"是两件事——响应码说 daemon 放没放行，退出码说命令跑没跑成。
 * 压成一个 int 必然丢信息，而丢的那部分恰好是排查时最想知道的。
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "boss_ipc.h"

#define JNI_FN(name) Java_com_boss_manager_core_BossIpc_##name

static char *dup_jstring(JNIEnv *env, jstring s)
{
    if (!s) return NULL;
    const char *u = (*env)->GetStringUTFChars(env, s, NULL);
    if (!u) return NULL;
    char *out = strdup(u);
    (*env)->ReleaseStringUTFChars(env, s, u);
    return out;
}

JNIEXPORT jstring JNICALL JNI_FN(nativeExec)(JNIEnv *env, jobject thiz,
                                             jstring jCommand, jstring jShell,
                                             jstring jContext, jint uid, jint gid,
                                             jint flags, jint timeoutMs)
{
    (void)thiz;
    struct boss_ipc_opts o;
    memset(&o, 0, sizeof(o));

    char *cmd = dup_jstring(env, jCommand);
    char *sh = dup_jstring(env, jShell);
    char *ctx = dup_jstring(env, jContext);
    o.command = cmd;
    o.shell = sh;
    o.context = ctx;
    o.uid = (uint32_t)uid;
    o.gid = (uint32_t)gid;
    o.flags = (uint32_t)flags;
    o.timeout_ms = (int)timeoutMs;

    struct boss_ipc_result r;
    memset(&r, 0, sizeof(r));
    int rc = boss_ipc_run(&o, &r);

    free(cmd);
    free(sh);
    free(ctx);

    jstring ret;
    if (rc < 0) {
        ret = (*env)->NewStringUTF(env, "DOWN\n");
        return ret;
    }

    size_t cap = (r.out_len ? r.out_len : 0) + 128;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        boss_ipc_result_free(&r);
        return (*env)->NewStringUTF(env, "DOWN\n");
    }
    int n = snprintf(buf, cap, "CODE=%u EXIT=%d TMO=%d\n", r.code, r.exit_code,
                     r.timed_out);
    if (n < 0) n = 0;
    if ((size_t)n < cap && r.out) {
        size_t room = cap - (size_t)n - 1;
        size_t cp = r.out_len < room ? r.out_len : room;
        memcpy(buf + n, r.out, cp);
        buf[n + cp] = '\0';
    }
    ret = (*env)->NewStringUTF(env, buf);
    free(buf);
    boss_ipc_result_free(&r);
    return ret;
}

JNIEXPORT jboolean JNICALL JNI_FN(nativePing)(JNIEnv *env, jobject thiz)
{
    (void)env;
    (void)thiz;
    return boss_ipc_ping() == 0 ? JNI_TRUE : JNI_FALSE;
}
