#include <jni.h>
#include <android/log.h>

#include "extractor.hpp"

#include <exception>
#include <string>

#define LOG_TAG "ObsidianExtract"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" JNIEXPORT jboolean JNICALL
Java_com_obsidian_client_NativeExtract_nativeIsExtractAvailable(JNIEnv*, jclass) {
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_obsidian_client_NativeExtract_nativeExtractClassic(
        JNIEnv* env, jclass, jstring jMpqDir, jstring jDataOut) {
    if (!jMpqDir || !jDataOut) {
        return env->NewStringUTF("mpqDir and dataOut are required");
    }

    const char* mpqChars = env->GetStringUTFChars(jMpqDir, nullptr);
    const char* outChars = env->GetStringUTFChars(jDataOut, nullptr);
    std::string mpqDir = mpqChars ? mpqChars : "";
    std::string dataOut = outChars ? outChars : "";
    if (mpqChars) env->ReleaseStringUTFChars(jMpqDir, mpqChars);
    if (outChars) env->ReleaseStringUTFChars(jDataOut, outChars);

    if (mpqDir.empty() || dataOut.empty()) {
        return env->NewStringUTF("mpqDir and dataOut must be non-empty");
    }

    ALOGI("Extract classic: mpqDir=%s dataOut=%s", mpqDir.c_str(), dataOut.c_str());

    try {
        wowee::tools::Extractor::Options opts;
        opts.mpqDir = mpqDir;
        opts.outputDir = dataOut;
        opts.expansion = "classic";
        opts.expansionSubdir = true;
        opts.threads = 2; // Tab A9+ — keep I/O + CPU modest
        opts.verify = false;
        opts.verbose = true;
        opts.emitPng = false;
        opts.emitJsonDbc = false;
        opts.emitWom = false;
        opts.emitWob = false;
        opts.emitTerrain = false;
        opts.generateDbcCsv = false;
        opts.skipDbcExtraction = false;

        const bool ok = wowee::tools::Extractor::run(opts);
        if (!ok) {
            ALOGE("Extractor::run returned false");
            return env->NewStringUTF("Extractor failed — see logcat (ObsidianExtract / wowee)");
        }
        ALOGI("Extractor::run succeeded");
        return nullptr; // success
    } catch (const std::exception& ex) {
        ALOGE("Extract exception: %s", ex.what());
        return env->NewStringUTF(ex.what());
    } catch (...) {
        ALOGE("Extract unknown exception");
        return env->NewStringUTF("Unknown extract failure");
    }
}
