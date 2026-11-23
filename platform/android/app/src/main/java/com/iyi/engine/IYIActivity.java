package com.iyi.engine;

import android.app.NativeActivity;
import android.os.Build;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

public class IYIActivity extends NativeActivity {
    private View keyboardView;
    private volatile boolean imeVisible;
    // NativeActivity dlopens the library outside the class loader: load it here too so JNI finds our natives.
    static { System.loadLibrary("iyigame"); }
    public static native void nativeOnText(String text);
    public static native void nativeOnImeHeight(int height);

    private static boolean engineStarted;   // the engine (and embedded Python) runs once per process

    @Override protected void onCreate(Bundle state) {
        // A second activity in the same process (Samsung relaunches an app updated while in front)
        // would start the engine again and crash in Python: open it in a fresh process instead.
        if (engineStarted) {
            startActivity(new android.content.Intent(this, IYIActivity.class)
                .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK | android.content.Intent.FLAG_ACTIVITY_CLEAR_TASK));
            Runtime.getRuntime().exit(0);
        }
        engineStarted = true;
        // before super.onCreate starts native code, so Python never reads a not-yet-started "off"
        License.check(this, getString(com.iyiengine.shell.R.string.iyi_license_public_key).trim());
        super.onCreate(state);
        keyboardView = new View(this) {
            @Override public boolean onCheckIsTextEditor() { return true; }
            @Override public InputConnection onCreateInputConnection(EditorInfo info) {
                info.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;  // letters commit one by one
                info.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
                return new BaseInputConnection(this, true) {
                    @Override public boolean commitText(CharSequence text, int cursor) {
                        if (text != null && text.length() != 0) nativeOnText(text.toString());
                        getEditable().clear();
                        return true;
                    }
                    @Override public boolean finishComposingText() {
                        Editable text = getEditable();
                        if (text.length() != 0) nativeOnText(text.toString());
                        text.clear();
                        return true;
                    }
                    // Backspace / Enter go to the engine through the text queue: an IME's key events never
                    // reach NativeActivity's input queue.
                    @Override public boolean deleteSurroundingText(int before, int after) {
                        if (getEditable().length() != 0) return super.deleteSurroundingText(before, after);
                        for (int i = 0; i < before; i++) nativeOnText("\b");
                        return true;
                    }
                    @Override public boolean deleteSurroundingTextInCodePoints(int before, int after) {
                        return deleteSurroundingText(before, after);   // Gboard's Backspace path
                    }
                    @Override public boolean sendKeyEvent(KeyEvent event) {
                        int code = event.getKeyCode();
                        if (code != KeyEvent.KEYCODE_DEL && code != KeyEvent.KEYCODE_ENTER) return super.sendKeyEvent(event);
                        if (event.getAction() == KeyEvent.ACTION_DOWN) nativeOnText(code == KeyEvent.KEYCODE_DEL ? "\b" : "\n");
                        return true;
                    }
                    @Override public boolean performEditorAction(int action) {
                        nativeOnText("\n");
                        return true;
                    }
                };
            }
        };
        keyboardView.setFocusableInTouchMode(true);
        addContentView(keyboardView, new ViewGroup.LayoutParams(1, 1));
        if (Build.VERSION.SDK_INT >= 30)   // the engine lifts its canvas above the keyboard
            getWindow().getDecorView().setOnApplyWindowInsetsListener((view, insets) -> {
                imeVisible = insets.isVisible(WindowInsets.Type.ime());
                nativeOnImeHeight(insets.getInsets(WindowInsets.Type.ime()).bottom);
                return view.onApplyWindowInsets(insets);
            });
        // targetSdk 36 delivers the back gesture only to callbacks (no KEYCODE_BACK, no onBackPressed):
        // without one the system finishes the activity and the process exits.
        if (Build.VERSION.SDK_INT >= 33)
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::back);
        immersive();
        PlayServices.init(this);
    }

    @SuppressWarnings("deprecation")
    @Override public void onBackPressed() { back(); }   // API < 33
    // Back hides the keyboard, else reaches the game as Escape (through the text queue, see FrameInput).
    private void back() {
        if (imeVisible) showKeyboard(false);
        else nativeOnText("\u001b");
    }

    // Google Play bridge: thin forwards, read by the engine through JNI (engine.play).
    public boolean playAvailable() { return PlayServices.available(); }
    public void playUnlock(String achievementId) { PlayServices.unlock(this, achievementId); }
    public void playCloudWrite(String name, byte[] data, long modifiedMs) { PlayServices.cloudWrite(this, name, data, modifiedMs); }
    public void playCloudFetch(String name) { PlayServices.cloudFetch(this, name); }
    public Object[] playCloudResult(String name) { return PlayServices.cloudResult(name); }
    public String[] playCloudNames() { return PlayServices.cloudNames(); }
    public void playRequestReview() { PlayServices.requestReview(this); }
    public String playLicenseState() { return PlayServices.licenseState(); }

    public void showKeyboard(boolean show) {
        runOnUiThread(() -> {
            if (Build.VERSION.SDK_INT < 30) imeVisible = show;   // no IME insets before API 30: our own request is the best guess
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (show) {
                keyboardView.requestFocus();
                imm.showSoftInput(keyboardView, InputMethodManager.SHOW_IMPLICIT);
            } else {
                imm.hideSoftInputFromWindow(keyboardView.getWindowToken(), 0);
                keyboardView.clearFocus();
            }
        });
    }

    @SuppressWarnings("deprecation")
    private void immersive() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }
    @Override protected void onResume() { super.onResume(); immersive(); PlayServices.refresh(this); }
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) immersive();
    }
}
