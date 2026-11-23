package com.iyi.engine;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Base64;
import android.util.Log;

import com.android.vending.licensing.ILicenseResultListener;
import com.android.vending.licensing.ILicensingService;


import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.spec.X509EncodedKeySpec;

/**
 * Minimal Google Play licensing (LVL) client: binds the Play Store licensing service, verifies the
 * RSA signature of the answer and keeps the resulting state in PlayServices.license.
 *
 * Only a definite NOT_LICENSED answer means "denied". Network errors, a missing Play Store, a
 * timeout, a bind failure or an unreadable answer all mean "ok": a paying player is never locked out.
 */
final class License implements ServiceConnection {
    private static final String TAG = "iyi";
    private static final String SERVICE = "com.android.vending.licensing.ILicensingService";
    private static final String PLAY_STORE = "com.android.vending";
    private static final long TIMEOUT_MS = 10_000;

    private static final int LICENSED = 0x0;
    private static final int NOT_LICENSED = 0x1;
    private static final int LICENSED_OLD_KEY = 0x2;

    private final Activity activity;
    private final PublicKey key;
    private final long nonce = new SecureRandom().nextLong();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable timeout = () -> finish("ok");
    private boolean done;

    private License(Activity activity, String encodedKey) throws Exception {
        this.activity = activity;
        byte[] decoded = Base64.decode(encodedKey, Base64.DEFAULT);
        key = KeyFactory.getInstance("RSA").generatePublic(new X509EncodedKeySpec(decoded));
    }

    static void check(Activity activity, String encodedKey) {
        if (encodedKey.isEmpty()) return;   // no key: licence_state stays "off"
        PlayServices.license = "pending";
        try {
            License license = new License(activity, encodedKey);
            license.handler.postDelayed(license.timeout, TIMEOUT_MS);
            if (!activity.bindService(new Intent(SERVICE).setPackage(PLAY_STORE), license, Context.BIND_AUTO_CREATE))
                license.finish("ok");
        } catch (Exception e) {
            Log.w(TAG, "play licence key unusable: " + e);
            PlayServices.license = "ok";
        }
    }

    @Override public void onServiceConnected(ComponentName name, IBinder binder) {
        try {
            ILicensingService.Stub.asInterface(binder).checkLicense(nonce, activity.getPackageName(),
                new ILicenseResultListener.Stub() {
                    @Override public void verifyLicense(int code, String signed, String signature) {
                        verify(code, signed, signature);
                    }
                });
        } catch (Exception e) {
            finish("ok");
        }
    }

    @Override public void onServiceDisconnected(ComponentName name) {
        finish("ok");
    }

    private void verify(int code, String signedData, String signature) {
        if (code != LICENSED && code != NOT_LICENSED && code != LICENSED_OLD_KEY) {
            finish("ok");   // server / network / quota error: never deny
            return;
        }
        try {
            Signature sig = Signature.getInstance("SHA1withRSA");
            sig.initVerify(key);
            byte[] data = signedData.getBytes(StandardCharsets.UTF_8);
            sig.update(data);
            // LVL signed data: "code|nonce|packageName|versionCode|userId|timestamp[:extras]"
            String[] fields = signedData.split(":", 2)[0].split("\\|");
            boolean genuine = sig.verify(Base64.decode(signature, Base64.DEFAULT))
                && fields.length >= 6
                && Integer.parseInt(fields[0]) == code
                && Long.parseLong(fields[1]) == nonce
                && activity.getPackageName().equals(fields[2]);
            finish(genuine && code == NOT_LICENSED ? "denied" : "ok");
        } catch (Exception e) {
            finish("ok");   // unreadable or unverifiable answer: not a denial
        }
    }

    // Called from the binder callback and the main-thread timeout: the first answer wins, so a late
    // NOT_LICENSED can never overwrite the timeout's "ok" (and vice versa).
    private synchronized void finish(String state) {
        if (done) return;
        done = true;
        PlayServices.license = state;
        handler.removeCallbacks(timeout);
        try {
            activity.unbindService(this);
        } catch (IllegalArgumentException ignored) {
            // already unbound
        }
    }
}
