package com.csgosource.tests;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;
import java.io.FileInputStream;
import java.lang.reflect.Method;
import java.util.ArrayList;
import org.json.JSONArray;
import org.json.JSONObject;

/** Run through adb shell/app_process: sends real multi-pointer Android events to SDL. */
public final class TouchInput {
    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("Expected a JSON gesture file");
        String text;
        try (FileInputStream file = new FileInputStream(args[0])) {
            byte[] data = new byte[(int)file.getChannel().size()];
            int offset = 0;
            while (offset < data.length) {
                int n = file.read(data, offset, data.length-offset);
                if (n < 0) throw new IllegalArgumentException("Incomplete gesture file");
                offset += n;
            }
            text = new String(data, "UTF-8");
        }
        Class<?> cls;
        try { cls = Class.forName("android.hardware.input.InputManagerGlobal"); }
        catch (ClassNotFoundException e) { cls = Class.forName("android.hardware.input.InputManager"); }
        Object manager = cls.getMethod("getInstance").invoke(null);
        Method inject = cls.getMethod("injectInputEvent", InputEvent.class, int.class);
        ArrayList<Integer> ids = new ArrayList<>();
        ArrayList<MotionEvent.PointerCoords> coords = new ArrayList<>();
        JSONArray steps = new JSONArray(text);
        long downTime = 0;
        int sent = 0;
        for (int n = 0; n < steps.length(); ++n) {
            JSONObject step = steps.getJSONObject(n);
            if (step.has("wait")) { SystemClock.sleep(step.getLong("wait")); continue; }
            String action = step.getString("action");
            int id = step.optInt("id", 0), index = ids.indexOf(id), code;
            if (action.equals("down")) {
                if (index >= 0) throw new IllegalArgumentException("Pointer is already down");
                if (ids.isEmpty()) downTime = SystemClock.uptimeMillis();
                index = ids.size(); ids.add(id); coords.add(new MotionEvent.PointerCoords());
                code = ids.size() == 1 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_POINTER_DOWN | (index << 8);
            } else {
                if (index < 0) throw new IllegalArgumentException("Unknown pointer");
                code = action.equals("up") ? (ids.size() == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP | (index << 8))
                    : action.equals("cancel") ? MotionEvent.ACTION_CANCEL : MotionEvent.ACTION_MOVE;
            }
            MotionEvent.PointerCoords c = coords.get(index);
            c.x = (float)step.optDouble("x", c.x); c.y = (float)step.optDouble("y", c.y); c.pressure = 1; c.size = .08f;
            MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[ids.size()];
            for (int i=0; i<ids.size(); ++i) {
                props[i] = new MotionEvent.PointerProperties(); props[i].id = ids.get(i); props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            }
            MotionEvent event = MotionEvent.obtain(downTime, SystemClock.uptimeMillis(), code, ids.size(), props,
                coords.toArray(new MotionEvent.PointerCoords[0]), 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
            boolean ok = (Boolean)inject.invoke(manager, event, 2);
            event.recycle();
            if (!ok) throw new IllegalStateException("Android rejected touch event " + n);
            if (action.equals("cancel")) {ids.clear();coords.clear();}
            else if (action.equals("up")) {ids.remove(index);coords.remove(index);}
            ++sent;
        }
        if (!ids.isEmpty()) throw new IllegalStateException("Gesture must release every pointer");
        System.out.println("ANDROID_TOUCH_PASS: " + sent + " pointer events delivered");
    }
}
