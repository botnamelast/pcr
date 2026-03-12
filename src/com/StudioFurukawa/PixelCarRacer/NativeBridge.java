package com.StudioFurukawa.PixelCarRacer;

import android.util.Log;

public class NativeBridge {

    private static final String TAG = "PCRMOD";
    private static boolean loaded = false;

    static {
        try {
            System.loadLibrary("mod");
            loaded = true;
            Log.d(TAG, "libmod.so loaded!");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "libmod.so load FAILED: " + e.getMessage());
        }
    }

    public static boolean isLoaded() { return loaded; }

    // === Mod control ===
    public static native void startMod();
    public static native void stopMod();

    // === AutoShift settings ===
    public static native void setAutoShift(boolean enabled);
    public static native void setShiftRPM(float rpm);
    public static native void setShiftMode(int mode); // 0=AUTO 1=MANUAL
    public static native void setManualRPMs(
        float r12, float r23, float r34, float r45, float r56);

    // === NOS ===
    public static native void setNOS(boolean val);

    // === Read game values (untuk debug display di ModMenu) ===
    public static native float getRPM();
    public static native int   getGear();
    public static native boolean isRaceStarted();
}
