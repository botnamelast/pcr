#include <jni.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>
#include <stdint.h>

#define TAG       "PCRMOD"
#define TAG_RPM   "PCRMOD_RPM"
#define TAG_SHIFT "PCRMOD_SHIFT"

#define LOGI(...) __android_log_print(ANDROID_LOG_DEBUG, TAG,       __VA_ARGS__)
#define LRPM(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_RPM,   __VA_ARGS__)
#define LSFT(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_SHIFT,  __VA_ARGS__)

// ============================================================
// GMS2 RValue
// ============================================================
struct RValue {
    union {
        double  val;
        int32_t i32;
        void*   ptr;
    };
    int32_t flags;  // offset 8
    int32_t kind;   // offset 12  (0=real, 1=string, 2=array)
};

// ============================================================
// GMS2 Variable Descriptor (g_VAR_xxx = 16 bytes)
// offset 8 = slot index
// ============================================================
struct GMS2VarDesc {
    int32_t hash;
    int32_t type;
    int32_t slot;   // ← index ke global variable array
    int32_t flags;
};

// ============================================================
// Function pointers — dari readelf
// _Z22Variable_Global_GetVariiP6RValuebb @ 0x9fd4c4
// _Z22Variable_Global_SetVariiP6RValue   @ 0x9fd408
// ============================================================
typedef void (*FnGetVar)(int slot, int unk, RValue* out, bool b1, bool b2);
typedef void (*FnSetVar)(int slot, int unk, RValue* val);

static FnGetVar g_fnGetVar = nullptr;
static FnSetVar g_fnSetVar = nullptr;

// ============================================================
// Descriptors & slots
// ============================================================
static GMS2VarDesc* g_desc_RPM      = nullptr;
static GMS2VarDesc* g_desc_gear     = nullptr;
static GMS2VarDesc* g_desc_speed    = nullptr;
static GMS2VarDesc* g_desc_shiftup  = nullptr;
static GMS2VarDesc* g_desc_shiftdn  = nullptr;
static GMS2VarDesc* g_desc_nos      = nullptr;

static int32_t g_slot_RPM     = -1;
static int32_t g_slot_gear    = -1;
static int32_t g_slot_speed   = -1;
static int32_t g_slot_shiftup = -1;
static int32_t g_slot_shiftdn = -1;
static int32_t g_slot_nos     = -1;

// Mod state
static bool  g_AutoShift     = false;
static float g_ShiftRPM      = 9300.0f;
static int   g_ShiftMode     = 0;
static float g_Shift1to2     = 9300.0f;
static float g_Shift2to3     = 9400.0f;
static float g_Shift3to4     = 9400.0f;
static float g_Shift4to5     = 9500.0f;
static float g_Shift5to6     = 9500.0f;

static bool  g_RaceStarted   = false;
static int   g_PrevGear      = 0;
static long  g_LastShiftTime = 0;
static long  g_LastRpmLog    = 0;

static pthread_t g_Thread;
static bool g_ThreadRunning  = false;

// ============================================================
// Read / Write via official GMS2 functions
// ============================================================
static double readVar(int32_t slot) {
    if (!g_fnGetVar || slot < 0) return 0.0;
    RValue out = {};
    g_fnGetVar(slot, 0, &out, false, false);
    return out.val;
}

static void writeVar(int32_t slot, double val) {
    if (!g_fnSetVar || slot < 0) return;
    RValue rv = {};
    rv.val  = val;
    rv.kind = 0;
    g_fnSetVar(slot, 0, &rv);
}

static float getRPM()   { return (float)readVar(g_slot_RPM); }
static int   getGear()  { return (int)  readVar(g_slot_gear); }
static float getSpeed() { return (float)readVar(g_slot_speed); }

// ============================================================
// Init
// ============================================================
static bool initSymbols() {
    void* lib = dlopen("libyoyo.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) lib = dlopen("/data/app/com.StudioFurukawa.PixelCarRacer/lib/arm64/libyoyo.so",
                           RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { LOGI("dlopen FAILED: %s", dlerror()); return false; }
    LOGI("dlopen OK: %p", lib);

    // Fungsi getter/setter
    g_fnGetVar = (FnGetVar) dlsym(lib, "_Z22Variable_Global_GetVariiP6RValuebb");
    g_fnSetVar = (FnSetVar) dlsym(lib, "_Z22Variable_Global_SetVariiP6RValue");
    LOGI("fnGetVar=%p fnSetVar=%p", g_fnGetVar, g_fnSetVar);

    // Descriptors
    g_desc_RPM     = (GMS2VarDesc*) dlsym(lib, "g_VAR_RPM");
    g_desc_gear    = (GMS2VarDesc*) dlsym(lib, "g_VAR_currentgear");
    g_desc_speed   = (GMS2VarDesc*) dlsym(lib, "g_VAR_car_speed");
    g_desc_shiftup = (GMS2VarDesc*) dlsym(lib, "g_VAR_vk_Shiftup");
    g_desc_shiftdn = (GMS2VarDesc*) dlsym(lib, "g_VAR_vk_Shiftdown");
    g_desc_nos     = (GMS2VarDesc*) dlsym(lib, "g_VAR_nos_enabled");

    // Baca slot dari descriptor
    if (g_desc_RPM)     g_slot_RPM     = g_desc_RPM->slot;
    if (g_desc_gear)    g_slot_gear    = g_desc_gear->slot;
    if (g_desc_speed)   g_slot_speed   = g_desc_speed->slot;
    if (g_desc_shiftup) g_slot_shiftup = g_desc_shiftup->slot;
    if (g_desc_shiftdn) g_slot_shiftdn = g_desc_shiftdn->slot;
    if (g_desc_nos)     g_slot_nos     = g_desc_nos->slot;

    LOGI("slots: RPM=%d gear=%d speed=%d shiftup=%d shiftdn=%d nos=%d",
         g_slot_RPM, g_slot_gear, g_slot_speed,
         g_slot_shiftup, g_slot_shiftdn, g_slot_nos);

    return g_fnGetVar != nullptr && g_fnSetVar != nullptr && g_desc_RPM != nullptr;
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
// Tick
// ============================================================
static void tick() {
    long  now   = currentTimeMs();
    float rpm   = getRPM();
    int   gear  = getGear();
    float speed = getSpeed();

    if (now - g_LastRpmLog >= 500) {
        LRPM("RPM=%.1f GEAR=%d SPEED=%.1f", rpm, gear, speed);
        g_LastRpmLog = now;
    }

    if (gear == 1 && g_PrevGear == 0) { g_RaceStarted = true;  LOGI("RACE START"); }
    if (gear == 0 && g_RaceStarted)   { g_RaceStarted = false; LOGI("RACE END"); }
    g_PrevGear = gear;

    if (!g_AutoShift || !g_RaceStarted) return;
    if (gear < 1 || gear >= 6) return;
    if (now - g_LastShiftTime < 300) return;

    float target = getTargetRPM(gear);
    if (rpm >= target) {
        LSFT("SHIFT UP! rpm=%.0f gear=%d target=%.0f", rpm, gear, target);
        writeVar(g_slot_shiftup, 1.0);
        g_LastShiftTime = now;
        usleep(50000);
        writeVar(g_slot_shiftup, 0.0);
    }
}

// ============================================================
// Mod thread
// ============================================================
static void* modThread(void*) {
    LOGI("Mod thread starting...");
    sleep(3);
    if (!initSymbols()) { LOGI("initSymbols FAILED"); return nullptr; }
    LOGI("Mod thread running!");
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
    writeVar(g_slot_nos, v ? 1.0 : 0.0);
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
