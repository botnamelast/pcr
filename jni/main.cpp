#include <jni.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>
#include <string.h>
#include <stdint.h>

#define TAG        "PCRMOD"
#define TAG_RPM    "PCRMOD_RPM"
#define TAG_SHIFT  "PCRMOD_SHIFT"
#define TAG_START  "PCRMOD_START"
#define TAG_DUMP   "PCRMOD_DUMP"

#define LOGI(...) __android_log_print(ANDROID_LOG_DEBUG, TAG,      __VA_ARGS__)
#define LRPM(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_RPM,  __VA_ARGS__)
#define LSFT(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_SHIFT, __VA_ARGS__)
#define LSTA(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_START, __VA_ARGS__)
#define LDMP(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_DUMP,  __VA_ARGS__)

// ============================================================
// GMS2 RValue struct
// Semua GMS2 variable disimpan sebagai RValue, bukan double langsung
// ============================================================
struct RValue {
    union {
        double  val;      // offset 0  — float/real value
        int32_t i32;      // offset 0  — int value
        void*   ptr;      // offset 0  — pointer value
    };
    int32_t flags;        // offset 8
    int32_t kind;         // offset 12
    // kind: 0=real/double, 1=string, 2=array, 3=ptr, 5=undefined, 13=int32
};

// ============================================================
// GMS2 variable pointers
// ============================================================
static RValue* g_RPM          = nullptr;
static RValue* g_CurrentGear  = nullptr;
static RValue* g_CarSpeed     = nullptr;
static RValue* g_VkShiftUp    = nullptr;
static RValue* g_VkShiftDown  = nullptr;
static RValue* g_NosEnabled   = nullptr;
static RValue* g_EngineStart  = nullptr;
static RValue* g_PeakHpRPM    = nullptr;

// ============================================================
// Mod state
// ============================================================
static bool  g_ModEnabled     = false;
static bool  g_AutoShift      = false;
static bool  g_NosButton      = false;
static float g_ShiftRPM       = 9300.0f;
static int   g_ShiftMode      = 0;
static float g_Shift1to2      = 9300.0f;
static float g_Shift2to3      = 9400.0f;
static float g_Shift3to4      = 9400.0f;
static float g_Shift4to5      = 9500.0f;
static float g_Shift5to6      = 9500.0f;

static bool  g_RaceStarted    = false;
static int   g_PrevGear       = 0;
static long  g_LastShiftTime  = 0;
static long  g_LastRpmLog     = 0;
static long  g_LastDump       = 0;
static bool  g_DumpDone       = false;

static pthread_t g_Thread;
static bool g_ThreadRunning   = false;

// ============================================================
// Dump raw bytes dari RValue pointer — untuk debug struktur
// ============================================================
static void dumpRValue(const char* name, RValue* rv) {
    if (!rv) {
        LDMP("%s = nullptr", name);
        return;
    }

    // Baca raw bytes
    uint8_t* raw = (uint8_t*)rv;

    // Interpret berbagai cara
    double   as_double  = *(double*)  (raw + 0);
    float    as_float0  = *(float*)   (raw + 0);
    float    as_float4  = *(float*)   (raw + 4);
    int32_t  as_int0    = *(int32_t*) (raw + 0);
    int32_t  as_int4    = *(int32_t*) (raw + 4);
    int32_t  as_int8    = *(int32_t*) (raw + 8);
    int32_t  as_int12   = *(int32_t*) (raw + 12);

    LDMP("%s @ %p", name, rv);
    LDMP("  double[0]  = %f", as_double);
    LDMP("  float[0]   = %f  float[4] = %f", as_float0, as_float4);
    LDMP("  int[0]     = %d  int[4]   = %d", as_int0, as_int4);
    LDMP("  int[8]     = %d  int[12]  = %d (flags/kind)", as_int8, as_int12);
    LDMP("  hex: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
        raw[0],raw[1],raw[2],raw[3],
        raw[4],raw[5],raw[6],raw[7],
        raw[8],raw[9],raw[10],raw[11],
        raw[12],raw[13],raw[14],raw[15]);
}

// ============================================================
// Init
// ============================================================
static bool initSymbols() {
    void* lib = dlopen("libyoyo.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        lib = dlopen("/data/app/com.StudioFurukawa.PixelCarRacer/lib/arm64/libyoyo.so",
                     RTLD_NOW | RTLD_GLOBAL);
    }
    if (!lib) {
        LOGI("dlopen libyoyo.so FAILED: %s", dlerror());
        return false;
    }
    LOGI("dlopen libyoyo.so OK: %p", lib);

    g_RPM         = (RValue*) dlsym(lib, "g_VAR_RPM");
    g_CurrentGear = (RValue*) dlsym(lib, "g_VAR_currentgear");
    g_CarSpeed    = (RValue*) dlsym(lib, "g_VAR_car_speed");
    g_VkShiftUp   = (RValue*) dlsym(lib, "g_VAR_vk_Shiftup");
    g_VkShiftDown = (RValue*) dlsym(lib, "g_VAR_vk_Shiftdown");
    g_NosEnabled  = (RValue*) dlsym(lib, "g_VAR_nos_enabled");
    g_EngineStart = (RValue*) dlsym(lib, "g_VAR_engine_start");
    g_PeakHpRPM   = (RValue*) dlsym(lib, "g_VAR_peak_hp_RPM");

    LOGI("g_RPM         = %p", g_RPM);
    LOGI("g_CurrentGear = %p", g_CurrentGear);
    LOGI("g_CarSpeed    = %p", g_CarSpeed);
    LOGI("g_VkShiftUp   = %p", g_VkShiftUp);
    LOGI("g_VkShiftDown = %p", g_VkShiftDown);
    LOGI("g_NosEnabled  = %p", g_NosEnabled);
    LOGI("g_EngineStart = %p", g_EngineStart);
    LOGI("g_PeakHpRPM   = %p", g_PeakHpRPM);

    return (g_RPM != nullptr && g_CurrentGear != nullptr);
}

// ============================================================
// Helper read — baca double dari RValue
// ============================================================
static float readRVal(RValue* rv) {
    if (!rv) return 0.0f;
    return (float)(rv->val);
}

static float getRPM()   { return readRVal(g_RPM); }
static int   getGear()  { return g_CurrentGear ? (int)(g_CurrentGear->val) : 0; }
static float getSpeed() { return readRVal(g_CarSpeed); }

static void setRVal(RValue* rv, double val) {
    if (!rv) return;
    rv->val  = val;
    rv->kind = 0; // real
}

static void setShiftUp(bool v)   { setRVal(g_VkShiftUp,   v ? 1.0 : 0.0); }
static void setShiftDown(bool v) { setRVal(g_VkShiftDown,  v ? 1.0 : 0.0); }
static void setNOS(bool v)       { setRVal(g_NosEnabled,   v ? 1.0 : 0.0); }

static long currentTimeMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static float getTargetRPM(int gear) {
    if (g_ShiftMode == 0) return g_ShiftRPM;
    switch (gear) {
        case 1: return g_Shift1to2;
        case 2: return g_Shift2to3;
        case 3: return g_Shift3to4;
        case 4: return g_Shift4to5;
        case 5: return g_Shift5to6;
        default: return g_ShiftRPM;
    }
}

// ============================================================
// Tick
// ============================================================
static void tick() {
    long now = currentTimeMs();

    // Dump raw bytes sekali setelah 5 detik pertama berjalan
    // Supaya game sudah loading dan nilai tidak semua 0
    if (!g_DumpDone && now - g_LastDump >= 5000) {
        LDMP("=== RAW DUMP (5s after init) ===");
        dumpRValue("g_VAR_RPM",         g_RPM);
        dumpRValue("g_VAR_currentgear", g_CurrentGear);
        dumpRValue("g_VAR_car_speed",   g_CarSpeed);
        dumpRValue("g_VAR_engine_start",g_EngineStart);
        dumpRValue("g_VAR_peak_hp_RPM", g_PeakHpRPM);
        LDMP("=== END DUMP ===");
        // Dump lagi tiap 10 detik (maksimal 3x total)
        g_LastDump = now;
        static int dumpCount = 0;
        dumpCount++;
        if (dumpCount >= 3) g_DumpDone = true;
    }

    float rpm   = getRPM();
    int   gear  = getGear();
    float speed = getSpeed();

    // Log RPM tiap 500ms
    if (now - g_LastRpmLog >= 500) {
        LRPM("RPM=%.1f GEAR=%d SPEED=%.1f", rpm, gear, speed);
        g_LastRpmLog = now;
    }

    // Deteksi race
    if (gear == 1 && g_PrevGear == 0) {
        g_RaceStarted = true;
        LSTA("RACE STARTED!");
    }
    if (gear == 0 && g_RaceStarted) {
        g_RaceStarted = false;
        LSTA("RACE ENDED");
    }
    g_PrevGear = gear;

    // Auto shift
    if (!g_AutoShift || !g_RaceStarted) return;
    if (gear < 1 || gear >= 6) return;
    if (now - g_LastShiftTime < 300) return;

    float target = getTargetRPM(gear);
    if (rpm >= target) {
        LSFT("SHIFT UP! rpm=%.0f gear=%d target=%.0f", rpm, gear, target);
        setShiftUp(true);
        g_LastShiftTime = now;
        usleep(50000);
        setShiftUp(false);
    }
}

// ============================================================
// Mod thread
// ============================================================
static void* modThread(void*) {
    LOGI("Mod thread starting, init symbols...");
    sleep(3);

    if (!initSymbols()) {
        LOGI("initSymbols FAILED");
        return nullptr;
    }

    g_LastDump = currentTimeMs(); // mulai timer dump
    LOGI("Mod thread running! Will dump RValue in 5s...");

    while (g_ThreadRunning) {
        tick();
        usleep(16000);
    }
    LOGI("Mod thread stopped!");
    return nullptr;
}

// ============================================================
// JNI
// ============================================================
extern "C" {

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_startMod(JNIEnv*, jclass) {
    if (g_ThreadRunning) return;
    g_ThreadRunning = true;
    g_ModEnabled    = true;
    pthread_create(&g_Thread, nullptr, modThread, nullptr);
    LOGI("startMod called!");
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_stopMod(JNIEnv*, jclass) {
    g_ThreadRunning = false;
    g_ModEnabled    = false;
    LOGI("stopMod called!");
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setAutoShift(JNIEnv*, jclass, jboolean enabled) {
    g_AutoShift = enabled;
    LOGI("setAutoShift: %d", (int)enabled);
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setShiftRPM(JNIEnv*, jclass, jfloat rpm) {
    g_ShiftRPM = rpm;
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setShiftMode(JNIEnv*, jclass, jint mode) {
    g_ShiftMode = mode;
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setManualRPMs(JNIEnv*, jclass,
        jfloat r12, jfloat r23, jfloat r34, jfloat r45, jfloat r56) {
    g_Shift1to2 = r12; g_Shift2to3 = r23; g_Shift3to4 = r34;
    g_Shift4to5 = r45; g_Shift5to6 = r56;
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setNOS(JNIEnv*, jclass, jboolean val) {
    setNOS(val);
}

JNIEXPORT jfloat JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_getRPM(JNIEnv*, jclass) {
    return getRPM();
}

JNIEXPORT jint JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_getGear(JNIEnv*, jclass) {
    return getGear();
}

JNIEXPORT jboolean JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_isRaceStarted(JNIEnv*, jclass) {
    return g_RaceStarted;
}

} // extern "C"
