#include "stdint.h"
#include "stddef.h"
#include "string.h"
#include "stdio.h"
#include "fcntl.h"
#include "unistd.h"

#include "android/log.h"
#include "zygisk.hpp"

#define LOG_TAG "GhostZygiskWV"
#define LOGD(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static uint8_t g_cached_widevine_id[32];
static bool g_cached_valid = false;
static jbyteArray (*orig_getPropertyByteArray)(JNIEnv *env, jobject thiz, jstring jname) = nullptr;

static void load_widevine_id_root() {
    if (g_cached_valid) return;
    int fd = open("/proc/ghost_widevine_raw", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, g_cached_widevine_id, sizeof(g_cached_widevine_id));
        close(fd);
        if (n == 32) {
            g_cached_valid = true;
            return;
        }
    }

    // Fallback: Parse hex text /proc/ghost_widevine
    fd = open("/proc/ghost_widevine", O_RDONLY);
    if (fd >= 0) {
        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *p = strstr(buf, "widevine_device_id:");
            if (p) {
                p += 19;
                while (*p == ' ') p++;
                if (strlen(p) >= 64) {
                    for (int i = 0; i < 32; i++) {
                        unsigned int val = 0;
                        sscanf(p + (i * 2), "%02x", &val);
                        g_cached_widevine_id[i] = (uint8_t)val;
                    }
                    g_cached_valid = true;
                }
            }
        }
    }
}

static jbyteArray hooked_getPropertyByteArray(JNIEnv *env, jobject thiz, jstring jname) {
    if (jname != nullptr) {
        const char *name = env->GetStringUTFChars(jname, nullptr);
        if (name != nullptr) {
            bool is_device_id = (strcmp(name, "deviceUniqueId") == 0);
            env->ReleaseStringUTFChars(jname, name);

            if (is_device_id) {
                if (!g_cached_valid) {
                    load_widevine_id_root();
                }

                if (g_cached_valid) {
                    LOGD("Successfully intercepted MediaDrm.getPropertyByteArray(deviceUniqueId) -> 32 bytes returned");
                    jbyteArray result = env->NewByteArray(32);
                    if (result != nullptr) {
                        env->SetByteArrayRegion(result, 0, 32, (const jbyte*)g_cached_widevine_id);
                        return result;
                    }
                } else {
                    LOGE("Failed to read /proc/ghost_widevine_raw, falling back to original");
                }
            }
        }
    }

    if (orig_getPropertyByteArray != nullptr) {
        return orig_getPropertyByteArray(env, thiz, jname);
    }
    return nullptr;
}

class GhostWidevineModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        load_widevine_id_root();
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        load_widevine_id_root();
        JNINativeMethod methods[] = {
            { (char*)"getPropertyByteArray", (char*)"(Ljava/lang/String;)[B", (void*) hooked_getPropertyByteArray },
        };
        api->hookJniNativeMethods(env, "android/media/MediaDrm", methods, 1);
        *(void **) &orig_getPropertyByteArray = methods[0].fnPtr;
        if (orig_getPropertyByteArray != nullptr) {
            LOGD("Zygisk Native Hook installed for android/media/MediaDrm.getPropertyByteArray");
        }
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
};

REGISTER_ZYGISK_MODULE(GhostWidevineModule)
