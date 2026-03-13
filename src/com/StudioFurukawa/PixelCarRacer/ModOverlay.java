package com.StudioFurukawa.PixelCarRacer;

import android.app.Service;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.os.IBinder;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.TextView;

public class ModOverlay extends Service {

    private WindowManager wm;
    private View overlay;
    private Handler handler = new Handler(Looper.getMainLooper());

    private TextView tvRPM;
    private TextView tvVtableStatus;
    private TextView tvCInstanceStatus;
    private TextView tvBtnVtable;
    private TextView tvBtnCInstance;

    private boolean vtableOn     = false;
    private boolean cinstanceOn  = false;

    private final Runnable rpmUpdater = new Runnable() {
        @Override public void run() {
            float  rpm = NativeBridge.getRPM();
            String src = NativeBridge.getRPMSource();
            tvRPM.setText(String.format("RPM: %.0f  [%s]", rpm, src));
            handler.postDelayed(this, 100);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        buildOverlay();
        NativeBridge.startMod();
    }

    private void buildOverlay() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.argb(210, 10, 10, 10));
        root.setPadding(28, 18, 28, 18);

        // Title
        root.addView(makeText("⚙ PCR MOD", 13, Color.parseColor("#FF6B00"), true));
        addDivider(root);

        // RPM display
        tvRPM = makeText("RPM: 0  [none]", 20, Color.parseColor("#00FF88"), true);
        root.addView(tvRPM);
        addDivider(root);

        // Vtable Hook section
        root.addView(makeText("Vtable Hook", 11, Color.LTGRAY, false));
        tvVtableStatus = makeText("● OFF", 11, Color.RED, false);
        root.addView(tvVtableStatus);
        tvBtnVtable = makeButton("[ ENABLE ]");
        tvBtnVtable.setOnClickListener(v -> toggleVtable());
        root.addView(tvBtnVtable);
        addDivider(root);

        // CInstance Read section
        root.addView(makeText("CInstance Read", 11, Color.LTGRAY, false));
        tvCInstanceStatus = makeText("● OFF", 11, Color.RED, false);
        root.addView(tvCInstanceStatus);
        tvBtnCInstance = makeButton("[ ENABLE ]");
        tvBtnCInstance.setOnClickListener(v -> toggleCInstance());
        root.addView(tvBtnCInstance);
        addDivider(root);

        // Close button
        TextView btnClose = makeButton("[ CLOSE ]");
        btnClose.setTextColor(Color.parseColor("#FF4444"));
        btnClose.setOnClickListener(v -> {
            NativeBridge.stopMod();
            stopSelf();
        });
        root.addView(btnClose);

        // Drag support
        WindowManager.LayoutParams params = makeParams();
        final int[] lastX = {0}, lastY = {0};
        root.setOnTouchListener((v, e) -> {
            switch (e.getAction()) {
                case MotionEvent.ACTION_DOWN:
                    lastX[0] = (int) e.getRawX();
                    lastY[0] = (int) e.getRawY();
                    break;
                case MotionEvent.ACTION_MOVE:
                    params.x += (int) e.getRawX() - lastX[0];
                    params.y += (int) e.getRawY() - lastY[0];
                    lastX[0] = (int) e.getRawX();
                    lastY[0] = (int) e.getRawY();
                    wm.updateViewLayout(overlay, params);
                    break;
            }
            return false;
        });

        overlay = root;
        wm.addView(overlay, params);
        handler.post(rpmUpdater);
    }

    private void toggleVtable() {
        vtableOn = !vtableOn;
        NativeBridge.setVtableHook(vtableOn);
        if (vtableOn) {
            tvVtableStatus.setText("● ON");
            tvVtableStatus.setTextColor(Color.GREEN);
            tvBtnVtable.setText("[ DISABLE ]");
            if (cinstanceOn) toggleCInstance();
        } else {
            tvVtableStatus.setText("● OFF");
            tvVtableStatus.setTextColor(Color.RED);
            tvBtnVtable.setText("[ ENABLE ]");
        }
    }

    private void toggleCInstance() {
        cinstanceOn = !cinstanceOn;
        NativeBridge.setCInstanceRead(cinstanceOn);
        if (cinstanceOn) {
            tvCInstanceStatus.setText("● ON");
            tvCInstanceStatus.setTextColor(Color.GREEN);
            tvBtnCInstance.setText("[ DISABLE ]");
            if (vtableOn) toggleVtable();
        } else {
            tvCInstanceStatus.setText("● OFF");
            tvCInstanceStatus.setTextColor(Color.RED);
            tvBtnCInstance.setText("[ ENABLE ]");
        }
    }

    private TextView makeText(String text, int sp, int color, boolean bold) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(sp);
        tv.setTextColor(color);
        tv.setTypeface(null, bold ? Typeface.BOLD : Typeface.NORMAL);
        tv.setPadding(0, 4, 0, 4);
        return tv;
    }

    private TextView makeButton(String text) {
        TextView tv = makeText(text, 12, Color.parseColor("#FFD700"), false);
        tv.setPadding(0, 10, 0, 10);
        return tv;
    }

    private void addDivider(LinearLayout parent) {
        View d = new View(this);
        d.setBackgroundColor(Color.argb(80, 255, 255, 255));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 1);
        lp.setMargins(0, 8, 0, 8);
        parent.addView(d, lp);
    }

    private WindowManager.LayoutParams makeParams() {
        int type = android.os.Build.VERSION.SDK_INT >= 26
            ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
            : WindowManager.LayoutParams.TYPE_PHONE;
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
            300,
            WindowManager.LayoutParams.WRAP_CONTENT,
            type,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        );
        p.gravity = Gravity.TOP | Gravity.START;
        p.x = 20;
        p.y = 120;
        return p;
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onDestroy() {
        handler.removeCallbacks(rpmUpdater);
        if (overlay != null) wm.removeView(overlay);
        super.onDestroy();
    }
}
