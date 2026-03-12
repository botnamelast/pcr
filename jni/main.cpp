#include <jni.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>
#include <string>

#define TAG        "PCRMOD"
#define TAG_RPM    "PCRMOD_RPM"
#define TAG_SHIFT  "PCRMOD_SHIFT"
#define TAG_START  "PCRMOD_START"

#define LOGI(...) __android_log_print(ANDROID_LOG_DEBUG, TAG,       __VA_ARGS__)
#define LRPM(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_RPM,   __VA_ARGS__)
#define LSFT(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_SHIFT,  __VA_ARGS__)
#define LSTA(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_START,  __VA_ARGS__)

// ============================================================
// GMS2 variable pointers (via dlsym)
// ============================================================
static double* g_RPM          = nullptr;
static double* g_CurrentGear  = nullptr;
static double* g_CarSpeed     = nullptr;
static double* g_VkShiftUp    = nullptr;
static double* g_VkShiftDown  = nullptr;
static double* g_NosEnabled   = nullptr;
static double* g_EngineStart  = nullptr;
static double* g_PeakHpRPM    = nullptr;

// ============================================================
// Mod state
// ============================================================
static bool g_ModEnabled      = false;
static bool g_AutoShift       = false;
static bool g_NosButton       = false;
static float g_ShiftRPM       = 9300.0f;
static int  g_ShiftMode       = 0; // 0=AUTO 1=MANUAL
static float g_Shift1to2      = 9300.0f;
static float g_Shift2to3      = 9400.0f;
static float g_Shift3to4      = 9400.0f;
static float g_Shift4to5      = 9500.0f;
static float g_Shift5to6      = 9500.0f;

static bool  g_RaceStarted    = false;
static int   g_PrevGear       = 0;
static long  g_LastShiftTime  = 0;
static long  g_LastRpmLog     = 0;

static pthread_t g_Thread;
static bool g_ThreadRunning   = false;

// ============================================================
// Init — cari semua symbol dari libyoyo.so
// ============================================================
static bool initSymbols() {
    void* lib = dlopen("libyoyo.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        // Coba path lengkap
        lib = dlopen("/data/app/com.StudioFurukawa.PixelCarRacer/lib/arm64/libyoyo.so",
                     RTLD_NOW | RTLD_GLOBAL);
    }
    if (!lib) {
        LOGI("dlopen libyoyo.so FAILED: %s", dlerror());
        return false;
    }
    LOGI("dlopen libyoyo.so OK: %p", lib);

    g_RPM         = (double*) dlsym(lib, "g_VAR_RPM");
    g_CurrentGear = (double*) dlsym(lib, "g_VAR_currentgear");
    g_CarSpeed    = (double*) dlsym(lib, "g_VAR_car_speed");
    g_VkShiftUp   = (double*) dlsym(lib, "g_VAR_vk_Shiftup");
    g_VkShiftDown = (double*) dlsym(lib, "g_VAR_vk_Shiftdown");
    g_NosEnabled  = (double*) dlsym(lib, "g_VAR_nos_enabled");
    g_EngineStart = (double*) dlsym(lib, "g_VAR_engine_start");
    g_PeakHpRPM   = (double*) dlsym(lib, "g_VAR_peak_hp_RPM");

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
// Helper read/write
// ============================================================
static float getRPM()      { return g_RPM         ? (float)*g_RPM         : 0.0f; }
static int   getGear()     { return g_CurrentGear ? (int)*g_CurrentGear   : 0; }
static float getSpeed()    { return g_CarSpeed     ? (float)*g_CarSpeed    : 0.0f; }

static void setShiftUp(bool val) {
    if (g_VkShiftUp) *g_VkShiftUp = val ? 1.0 : 0.0;
}
static void setShiftDown(bool val) {
    if (g_VkShiftDown) *g_VkShiftDown = val ? 1.0 : 0.0;
}
static void setNOS(bool val) {
    if (g_NosEnabled) *g_NosEnabled = val ? 1.0 : 0.0;
}

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
// Tick — dipanggil tiap loop
// ============================================================
static void tick() {
    float rpm   = getRPM();
    int   gear  = getGear();
    float speed = getSpeed();

    // Log RPM tiap 500ms
    long now = currentTimeMs();
    if (now - g_LastRpmLog >= 500) {
        LRPM("RPM=%d GEAR=%d SPEED=%.1f", (int)rpm, gear, speed);
        g_LastRpmLog = now;
    }

    // Deteksi race start
    if (gear == 1 && g_PrevGear == 0) {
        g_RaceStarted = true;
        LSTA("RACE STARTED! rpm=%d gear=%d", (int)rpm, gear);
    }
    if (gear == 0 && g_RaceStarted) {
        g_RaceStarted = false;
        LSTA("RACE ENDED/RESET");
    }
    g_PrevGear = gear;

    // Auto shift logic
    if (!g_AutoShift || !g_RaceStarted) return;
    if (gear < 1 || gear >= 6) return;
    if (now - g_LastShiftTime < 300) return;

    float target = getTargetRPM(gear);
    if (rpm >= target) {
        LSFT("SHIFT UP! rpm=%d gear=%d target=%d", (int)rpm, gear, (int)target);
        setShiftUp(true);
        g_LastShiftTime = now;
        usleep(50000); // 50ms
        setShiftUp(false);
    }
}

// ============================================================
// Mod thread
// ============================================================
static void* modThread(void*) {
    LOGI("Mod thread starting, init symbols...");

    // Tunggu game load dulu
    sleep(3);

    if (!initSymbols()) {
        LOGI("initSymbols FAILED, thread exit");
        return nullptr;
    }

    LOGI("Mod thread running!");
    while (g_ThreadRunning) {
        tick();
        usleep(16000); // ~60fps
    }

    LOGI("Mod thread stopped!");
    return nullptr;
}

// ============================================================
// JNI — dipanggil dari Java ModMenu / NativeBridge
// ============================================================
extern "C" {

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_startMod(
        JNIEnv*, jclass) {
    if (g_ThreadRunning) return;
    g_ThreadRunning = true;
    g_ModEnabled    = true;
    pthread_create(&g_Thread, nullptr, modThread, nullptr);
    LOGI("startMod called!");
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_stopMod(
        JNIEnv*, jclass) {
    g_ThreadRunning = false;
    g_ModEnabled    = false;
    LOGI("stopMod called!");
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setAutoShift(
        JNIEnv*, jclass, jboolean enabled) {
    g_AutoShift = enabled;
    LOGI("setAutoShift: %d", (int)enabled);
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setShiftRPM(
        JNIEnv*, jclass, jfloat rpm) {
    g_ShiftRPM = rpm;
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setShiftMode(
        JNIEnv*, jclass, jint mode) {
    g_ShiftMode = mode;
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setManualRPMs(
        JNIEnv*, jclass,
        jfloat r12, jfloat r23, jfloat r34, jfloat r45, jfloat r56) {
    g_Shift1to2 = r12;
    g_Shift2to3 = r23;
    g_Shift3to4 = r34;
    g_Shift4to5 = r45;
    g_Shift5to6 = r56;
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setNOS(
        JNIEnv*, jclass, jboolean val) {
    setNOS(val);
}

JNIEXPORT jfloat JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_getRPM(
        JNIEnv*, jclass) {
    return getRPM();
}

JNIEXPORT jint JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_getGear(
        JNIEnv*, jclass) {
    return getGear();
}

JNIEXPORT jboolean JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_isRaceStarted(
        JNIEnv*, jclass) {
    return g_RaceStarted;
}

} // extern "C"
