#include <jni.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>
#include <stdint.h>
#include <math.h>

#define TAG      "PCRMOD"
#define TAG_RPM  "PCRMOD_RPM"
#define TAG_SCAN "PCRMOD_SCAN"

#define LOGI(...) __android_log_print(ANDROID_LOG_DEBUG, TAG,      __VA_ARGS__)
#define LRPM(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_RPM,  __VA_ARGS__)
#define LSCN(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_SCAN, __VA_ARGS__)

// ============================================================
// g_VAR_xxx = pointer ke sesuatu. Kita scan offset 0,8,16,24,32
// untuk cari double yang nilainya masuk akal (RPM: 0-12000)
// ============================================================
static void** g_RPM_pp         = nullptr;
static void** g_CurrentGear_pp = nullptr;
static void** g_CarSpeed_pp    = nullptr;
static void** g_VkShiftUp_pp   = nullptr;
static void** g_VkShiftDown_pp = nullptr;
static void** g_NosEnabled_pp  = nullptr;

static bool  g_AutoShift     = false;
static float g_ShiftRPM      = 9300.0f;
static int   g_ShiftMode     = 0;
static float g_Shift1to2     = 9300.0f;
static float g_Shift2to3     = 9400.0f;
static float g_Shift3to4     = 9400.0f;
static float g_Shift4to5     = 9500.0f;
static float g_Shift5to6     = 9500.0f;

// Offset yang bekerja (ditemukan dari scan)
static int g_RPM_offset  = -1;
static int g_Gear_offset = -1;

static bool  g_RaceStarted   = false;
static int   g_PrevGear      = 0;
static long  g_LastShiftTime = 0;
static long  g_LastRpmLog    = 0;
static long  g_LastScan      = 0;
static bool  g_ScanDone      = false;

static pthread_t g_Thread;
static bool g_ThreadRunning  = false;

// ============================================================
// Baca double dari void* + offset
// ============================================================
static double readDouble(void** pp, int offset) {
    if (!pp || !*pp) return -1.0;
    uint8_t* base = (uint8_t*)(*pp);
    double v;
    __builtin_memcpy(&v, base + offset, sizeof(double));
    return v;
}

static void writeDouble(void** pp, int offset, double val) {
    if (!pp || !*pp) return;
    uint8_t* base = (uint8_t*)(*pp);
    __builtin_memcpy(base + offset, &val, sizeof(double));
}

// ============================================================
// Scan semua offset untuk cari RPM yang masuk akal
// RPM valid: 500 - 12000
// Gear valid: 1 - 6
// ============================================================
static void scanOffsets() {
    if (!g_RPM_pp || !*g_RPM_pp) {
        LSCN("g_RPM_pp null, skip scan");
        return;
    }

    LSCN("=== OFFSET SCAN ===");
    LSCN("g_RPM_pp=%p  *g_RPM_pp=%p", g_RPM_pp, *g_RPM_pp);

    int offsets[] = {0, 4, 8, 12, 16, 20, 24, 32, 40, 48};
    for (int i = 0; i < 10; i++) {
        int off = offsets[i];
        double d = readDouble(g_RPM_pp, off);
        float  f;
        uint8_t* base = (uint8_t*)(*g_RPM_pp);
        __builtin_memcpy(&f, base + off, sizeof(float));
        int32_t iv;
        __builtin_memcpy(&iv, base + off, sizeof(int32_t));

        LSCN("RPM off+%02d: double=%.2f float=%.2f int=%d", off, d, f, iv);

        // Auto-detect offset RPM
        if (d >= 500.0 && d <= 12000.0 && g_RPM_offset == -1) {
            g_RPM_offset = off;
            LSCN("*** RPM offset FOUND: +%d (val=%.1f) ***", off, d);
        }
    }

    // Scan gear juga
    if (g_CurrentGear_pp && *g_CurrentGear_pp) {
        LSCN("--- GEAR SCAN ---");
        uint8_t* base = (uint8_t*)(*g_CurrentGear_pp);
        for (int i = 0; i < 10; i++) {
            int off = offsets[i];
            double d = readDouble(g_CurrentGear_pp, off);
            int32_t iv;
            __builtin_memcpy(&iv, base + off, sizeof(int32_t));
            LSCN("GEAR off+%02d: double=%.2f int=%d", off, d, iv);

            if (d >= 1.0 && d <= 6.0 && g_Gear_offset == -1) {
                g_Gear_offset = off;
                LSCN("*** GEAR offset FOUND: +%d (val=%.1f) ***", off, d);
            }
        }
    }
    LSCN("=== END SCAN ===");
}

static float getRPM() {
    if (g_RPM_offset < 0) return 0.0f;
    return (float) readDouble(g_RPM_pp, g_RPM_offset);
}

static int getGear() {
    if (g_Gear_offset < 0) return 0;
    return (int) readDouble(g_CurrentGear_pp, g_Gear_offset);
}

// ============================================================
// Init
// ============================================================
static bool initSymbols() {
    void* lib = dlopen("libyoyo.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) lib = dlopen("/data/app/com.StudioFurukawa.PixelCarRacer/lib/arm64/libyoyo.so",
                           RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { LOGI("dlopen FAILED: %s", dlerror()); return false; }
    LOGI("dlopen OK: %p", lib);

    g_RPM_pp         = (void**) dlsym(lib, "g_VAR_RPM");
    g_CurrentGear_pp = (void**) dlsym(lib, "g_VAR_currentgear");
    g_CarSpeed_pp    = (void**) dlsym(lib, "g_VAR_car_speed");
    g_VkShiftUp_pp   = (void**) dlsym(lib, "g_VAR_vk_Shiftup");
    g_VkShiftDown_pp = (void**) dlsym(lib, "g_VAR_vk_Shiftdown");
    g_NosEnabled_pp  = (void**) dlsym(lib, "g_VAR_nos_enabled");

    LOGI("g_RPM_pp=%p -> *=%p", g_RPM_pp, g_RPM_pp ? *g_RPM_pp : nullptr);
    LOGI("g_Gear_pp=%p -> *=%p", g_CurrentGear_pp, g_CurrentGear_pp ? *g_CurrentGear_pp : nullptr);

    return g_RPM_pp != nullptr;
}

static long currentTimeMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ============================================================
// Tick
// ============================================================
static void tick() {
    long now = currentTimeMs();

    // Scan tiap 3 detik sampai offset ditemukan, maks 5x
    static int scanCount = 0;
    if (!g_ScanDone && now - g_LastScan >= 3000) {
        scanOffsets();
        g_LastScan = now;
        scanCount++;
        if ((g_RPM_offset >= 0 && g_Gear_offset >= 0) || scanCount >= 5)
            g_ScanDone = true;
    }

    float rpm   = getRPM();
    int   gear  = getGear();

    if (now - g_LastRpmLog >= 500) {
        LRPM("RPM=%.1f GEAR=%d (roff=%d goff=%d)", rpm, gear, g_RPM_offset, g_Gear_offset);
        g_LastRpmLog = now;
    }

    if (gear == 1 && g_PrevGear == 0) g_RaceStarted = true;
    if (gear == 0 && g_RaceStarted)   g_RaceStarted = false;
    g_PrevGear = gear;

    if (!g_AutoShift || !g_RaceStarted || g_RPM_offset < 0) return;
    if (gear < 1 || gear >= 6) return;
    if (now - g_LastShiftTime < 300) return;

    float target = (g_ShiftMode == 0) ? g_ShiftRPM : g_ShiftRPM;
    if (rpm >= target) {
        writeDouble(g_VkShiftUp_pp, g_RPM_offset, 1.0); // pakai offset sama dulu
        g_LastShiftTime = now;
        usleep(50000);
        writeDouble(g_VkShiftUp_pp, g_RPM_offset, 0.0);
    }
}

// ============================================================
// Mod thread
// ============================================================
static void* modThread(void*) {
    LOGI("Mod thread starting...");
    sleep(3);
    if (!initSymbols()) { LOGI("initSymbols FAILED"); return nullptr; }
    g_LastScan = currentTimeMs();
    LOGI("Mod thread running! Scanning offsets...");
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
    pthread_create(&g_Thread, nullptr, modThread, nullptr);
    LOGI("startMod called!");
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_stopMod(JNIEnv*, jclass) {
    g_ThreadRunning = false;
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setAutoShift(JNIEnv*, jclass, jboolean v) {
    g_AutoShift = v;
    LOGI("setAutoShift: %d", (int)v);
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
    g_Shift1to2=r12; g_Shift2to3=r23; g_Shift3to4=r34;
    g_Shift4to5=r45; g_Shift5to6=r56;
}

JNIEXPORT void JNICALL
Java_com_StudioFurukawa_PixelCarRacer_NativeBridge_setNOS(JNIEnv*, jclass, jboolean v) {
    writeDouble(g_NosEnabled_pp, g_RPM_offset >= 0 ? g_RPM_offset : 0, v ? 1.0 : 0.0);
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
