#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <atomic>
#include <cmath>
#include <cstdint>

#define TAG "PCRMOD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ============================================================
//  GMS2 Types
// ============================================================
struct RValue {
    union {
        double  real;
        int32_t i32;
        void*   ptr;
    };
    int32_t  flags;
    uint32_t kind;   // 0=real/double
};

struct VarDescriptor {
    int32_t hash;
    int32_t type;
    int32_t slot;    // raw ID (actual_slot + 100000)
    int32_t pad;
};

// ============================================================
//  Globals
// ============================================================
static void*     g_libyoyo   = nullptr;
static uintptr_t g_base      = 0;

static VarDescriptor* g_desc_RPM = nullptr;
static int32_t        g_slot_RPM = -1;

static constexpr int32_t GMS2_VAR_BASE = 100000;

static std::atomic<double> g_rpm_value{0.0};

enum RPMSource { SRC_NONE, SRC_VTABLE, SRC_CINSTANCE };
static std::atomic<int>  g_rpm_source{SRC_NONE};
static std::atomic<bool> g_vtable_hook_enabled{false};
static std::atomic<bool> g_cinstance_read_enabled{false};
static std::atomic<bool> g_mod_running{false};

// ============================================================
//  APPROACH 1: VTABLE HOOK
// ============================================================
typedef RValue* (*GetVarFn)(void* instance, int32_t varId);
static GetVarFn  g_original_GetVar    = nullptr;
static void**    g_vtable_patch_entry = nullptr;

static RValue* hooked_GetVar(void* instance, int32_t varId) {
    RValue* result = g_original_GetVar(instance, varId);
    if (g_vtable_hook_enabled.load(std::memory_order_relaxed)) {
        if (varId == 102586 && result) {
            double val = result->real;
            if (val >= 0.0 && val <= 25000.0) {
                g_rpm_value.store(val, std::memory_order_relaxed);
                g_rpm_source.store(SRC_VTABLE, std::memory_order_relaxed);
            }
        }
    }
    return result;
}

static bool installVtableHook(void* cinstance_ptr) {
    if (!cinstance_ptr) return false;
    void** vtable = *reinterpret_cast<void***>(cinstance_ptr);
    if (!vtable) return false;

    g_vtable_patch_entry = &vtable[2];
    g_original_GetVar    = reinterpret_cast<GetVarFn>(vtable[2]);

    uintptr_t page = reinterpret_cast<uintptr_t>(g_vtable_patch_entry) & ~(uintptr_t)4095;
    if (mprotect(reinterpret_cast<void*>(page), 8192,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("vtable mprotect failed: %s", strerror(errno));
        return false;
    }
    *g_vtable_patch_entry = reinterpret_cast<void*>(hooked_GetVar);
    LOGI("Vtable hook installed, original=0x%lx", (uintptr_t)g_original_GetVar);
    return true;
}

static void removeVtableHook() {
    if (!g_vtable_patch_entry || !g_original_GetVar) return;
    *g_vtable_patch_entry = reinterpret_cast<void*>(g_original_GetVar);
    g_original_GetVar    = nullptr;
    g_vtable_patch_entry = nullptr;
    LOGI("Vtable hook removed");
}

// ============================================================
//  APPROACH 2: CINSTANCE DIRECT READ
// ============================================================
static std::atomic<uintptr_t> g_vars_array_ptr{0};

static bool tryUpdateVarsArray() {
    if (!g_base) return false;
    uintptr_t* pptr = reinterpret_cast<uintptr_t*>(g_base + 0x15b92f8);
    if (!*pptr) return false;
    uintptr_t global_obj = *reinterpret_cast<uintptr_t*>(*pptr);
    if (!global_obj) return false;
    int32_t flag = *reinterpret_cast<int32_t*>(global_obj + 0x6c);
    if (!flag) return false;
    uintptr_t vars = *reinterpret_cast<uintptr_t*>(global_obj + 0x8);
    if (!vars) return false;
    g_vars_array_ptr.store(vars, std::memory_order_relaxed);
    return true;
}

static void readRPM_CInstance() {
    if (!g_cinstance_read_enabled.load(std::memory_order_relaxed)) return;
    uintptr_t vars = g_vars_array_ptr.load(std::memory_order_relaxed);
    if (!vars) { tryUpdateVarsArray(); return; }
    if (g_slot_RPM < 0) return;
    RValue* rv = reinterpret_cast<RValue*>(vars + g_slot_RPM * 16);
    if (rv->kind != 0 && rv->kind != 0xFFFFFF) return;
    double val = rv->real;
    if (val >= 0.0 && val <= 25000.0) {
        g_rpm_value.store(val, std::memory_order_relaxed);
        g_rpm_source.store(SRC_CINSTANCE, std::memory_order_relaxed);
    }
}

// ============================================================
//  INIT
// ============================================================
static bool initSymbols() {
    g_libyoyo = dlopen("libyoyo.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libyoyo) {
        LOGE("dlopen libyoyo failed: %s", dlerror());
        return false;
    }

    void* sym = dlsym(g_libyoyo, "g_VAR_RPM");
    if (!sym) { LOGE("g_VAR_RPM not found"); return false; }

    Dl_info info;
    dladdr(sym, &info);
    g_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    LOGI("libyoyo base: 0x%lx", g_base);

    g_desc_RPM = reinterpret_cast<VarDescriptor*>(sym);

    sleep(3);
    g_slot_RPM = g_desc_RPM->slot - GMS2_VAR_BASE;
    LOGI("RPM slot raw=%d actual=%d", g_desc_RPM->slot, g_slot_RPM);

    tryUpdateVarsArray();
    LOGI("vars_array=0x%lx", (uintptr_t)g_vars_array_ptr.load());
    return true;
}

// ============================================================
//  MOD THREAD
// ============================================================
static void* modThread(void*) {
    LOGI("Mod thread running!");
    if (!initSymbols()) { LOGE("init failed"); return nullptr; }

    int tick = 0;
    while (g_mod_running.load()) {
        readRPM_CInstance();
        if (++tick % 20 == 0) { tryUpdateVarsArray(); tick = 0; }
        usleep(50000);
    }
    LOGI("Mod thread exit");
    return nullptr;
}

// ============================================================
//  JNI — package: com.StudioFurukawa.PixelCarRacer
// ============================================================
#define JNI_FN(name) \
    Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_##name

extern "C" {

JNIEXPORT void JNICALL JNI_FN(startMod)(JNIEnv*, jclass) {
    if (g_mod_running.load()) return;
    g_mod_running.store(true);
    pthread_t t;
    pthread_create(&t, nullptr, modThread, nullptr);
    pthread_detach(t);
    LOGI("startMod called");
}

JNIEXPORT void JNICALL JNI_FN(stopMod)(JNIEnv*, jclass) {
    g_mod_running.store(false);
    removeVtableHook();
    g_vtable_hook_enabled.store(false);
    g_cinstance_read_enabled.store(false);
    g_rpm_source.store(SRC_NONE, std::memory_order_relaxed);
    LOGI("stopMod called");
}

JNIEXPORT jfloat JNICALL JNI_FN(getRPM)(JNIEnv*, jclass) {
    return (jfloat)g_rpm_value.load(std::memory_order_relaxed);
}

JNIEXPORT jstring JNICALL JNI_FN(getRPMSource)(JNIEnv* env, jclass) {
    switch (g_rpm_source.load(std::memory_order_relaxed)) {
        case SRC_VTABLE:    return env->NewStringUTF("vtable");
        case SRC_CINSTANCE: return env->NewStringUTF("cinstance");
        default:            return env->NewStringUTF("none");
    }
}

JNIEXPORT void JNICALL JNI_FN(setVtableHook)(JNIEnv*, jclass, jboolean enable) {
    if (enable) {
        if (!g_base) { LOGE("setVtableHook: not init yet"); return; }
        uintptr_t* ci_ptr = reinterpret_cast<uintptr_t*>(g_base + 0x15b92e8);
        void* ci = reinterpret_cast<void*>(*ci_ptr);
        if (ci && !g_original_GetVar) {
            if (installVtableHook(ci)) {
                g_vtable_hook_enabled.store(true);
                g_cinstance_read_enabled.store(false);
            }
        } else { LOGE("setVtableHook: no CInstance or already hooked"); }
    } else {
        g_vtable_hook_enabled.store(false);
        removeVtableHook();
        if (g_rpm_source.load() == SRC_VTABLE)
            g_rpm_source.store(SRC_NONE, std::memory_order_relaxed);
    }
}

JNIEXPORT void JNICALL JNI_FN(setCInstanceRead)(JNIEnv*, jclass, jboolean enable) {
    g_cinstance_read_enabled.store(enable);
    if (enable) {
        g_vtable_hook_enabled.store(false);
        removeVtableHook();
        tryUpdateVarsArray();
        LOGI("CInstance read ON, vars=0x%lx", (uintptr_t)g_vars_array_ptr.load());
    } else {
        if (g_rpm_source.load() == SRC_CINSTANCE)
            g_rpm_source.store(SRC_NONE, std::memory_order_relaxed);
    }
}

JNIEXPORT void JNICALL JNI_FN(setAutoShift)(JNIEnv*, jclass, jboolean) {}
JNIEXPORT void JNICALL JNI_FN(setShiftRPM)(JNIEnv*, jclass, jfloat) {}
JNIEXPORT void JNICALL JNI_FN(setShiftMode)(JNIEnv*, jclass, jint) {}
JNIEXPORT void JNICALL JNI_FN(setManualRPMs)(JNIEnv*, jclass, jfloat, jfloat, jfloat, jfloat, jfloat) {}
JNIEXPORT void JNICALL JNI_FN(setNOS)(JNIEnv*, jclass, jboolean) {}
JNIEXPORT jint JNICALL JNI_FN(getGear)(JNIEnv*, jclass) { return 0; }
JNIEXPORT jboolean JNICALL JNI_FN(isRaceStarted)(JNIEnv*, jclass) { return JNI_FALSE; }

} // extern "C"
