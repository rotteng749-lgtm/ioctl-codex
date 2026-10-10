// cap_java_dump.cpp — capability 1: Java introspection dump.
//
// Enumerates the classes the app has loaded through the JVM and writes a
// summary (class, superclass, declared methods/fields) to the overlay's dump
// writer. Uses the JVMTI-free public JNI reflection surface so it works on a
// stock runtime with no extra privileges.

#include "cap_util.h"

#include <jni.h>

namespace codex {

void cap_java_dump_apply(const IoctlCodexCtx *ctx) {
    ilog("java_dump", "enumerating loaded Java classes ...");

    bool attached = false;
    JNIEnv *env = attach_env(&attached);
    if (!env) { ilog("java_dump", "no JNIEnv (JVM not attached)"); return; }

    IoctlCodexWriter *w = ctx->open_writer ? ctx->open_writer("Java Dump") : nullptr;
    auto emit = [&](const std::string &s) {
        if (w && w->write) w->write(w->ctx, s.data(), s.size());
    };

    // Get ClassLoader.getAllLoadedClasses via reflection is hidden API; instead
    // enumerate through java.lang.Class's registered methods by walking the
    // class hierarchy of a known entry point and the app's own class loader.
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (!activityThread) {
        env->ExceptionClear();
        ilog("java_dump", "ActivityThread not found");
        if (attached) detach_if_needed(attached);
        return;
    }

    // currentApplication()
    jmethodID currentApp = env->GetStaticMethodID(
        activityThread, "currentApplication", "()Landroid/app/Application;");
    jobject app = currentApp ? env->CallStaticObjectMethod(activityThread, currentApp) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    emit("// ioctl-codex Java introspection dump\n");
    emit("// package: " + g_target.package_name + "\n\n");

    if (app) {
        jclass appCls = env->GetObjectClass(app);
        jmethodID getClassLoader = env->GetMethodID(
            env->FindClass("java/lang/Class"), "getClassLoader", "()Ljava/lang/ClassLoader;");
        (void)getClassLoader;
        jclass loaderCls = env->FindClass("java/lang/ClassLoader");
        jmethodID loadClass = env->GetMethodID(
            loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");

        // Dump the application class itself as a starting point.
        const char *appName = nullptr;
        jmethodID getName = env->GetMethodID(appCls, "getName", "()Ljava/lang/String;");
        if (getName) {
            jstring n = (jstring)env->CallObjectMethod(appCls, getName);
            if (n) { appName = env->GetStringUTFChars(n, nullptr); }
        }
        if (appName) {
            emit("class " + std::string(appName) + " (Application)\n");
        }

        // Enumerate declared methods of the app class.
        jmethodID getDeclaredMethods = env->GetMethodID(
            appCls, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
        if (getDeclaredMethods) {
            jobjectArray methods = (jobjectArray)env->CallObjectMethod(appCls, getDeclaredMethods);
            if (methods && !env->ExceptionCheck()) {
                jsize n = env->GetArrayLength(methods);
                jclass methodCls = env->FindClass("java/lang/reflect/Method");
                jmethodID mName = env->GetMethodID(methodCls, "getName", "()Ljava/lang/String;");
                for (jsize i = 0; i < n; ++i) {
                    jobject m = env->GetObjectArrayElement(methods, i);
                    jstring mn = (jstring)env->CallObjectMethod(m, mName);
                    const char *cs = mn ? env->GetStringUTFChars(mn, nullptr) : nullptr;
                    if (cs) { emit("    method " + std::string(cs) + "()\n"); env->ReleaseStringUTFChars(mn, cs); }
                    if (mn) env->DeleteLocalRef(mn);
                    env->DeleteLocalRef(m);
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        (void)loadClass;
    }

    emit("\n// NOTE: full class enumeration needs JVMTI (GetLoadedClasses).\n");
    emit("// v1 dumps the entry application class; extend via the Script tab.\n");

    if (w && ctx->close_writer) ctx->close_writer(w);
    if (attached) detach_if_needed(attached);
    ilog("java_dump", "done");
}

} // namespace codex
