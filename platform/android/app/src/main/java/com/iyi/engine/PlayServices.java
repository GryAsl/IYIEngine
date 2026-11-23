package com.iyi.engine;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;

import com.google.android.gms.common.api.ApiException;
import com.google.android.gms.games.GamesClientStatusCodes;
import com.google.android.gms.games.PlayGames;
import com.google.android.gms.games.PlayGamesSdk;
import com.google.android.gms.games.snapshot.Snapshot;
import com.google.android.gms.games.snapshot.SnapshotContents;
import com.google.android.gms.games.snapshot.SnapshotMetadataBuffer;
import com.google.android.gms.games.snapshot.SnapshotMetadataChange;
import com.google.android.gms.games.SnapshotsClient;
import com.google.android.gms.tasks.Task;
import com.google.android.play.core.review.ReviewManager;
import com.google.android.play.core.review.ReviewManagerFactory;
import com.iyiengine.shell.R;

import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The Google Play bridge behind engine.play: Play Games Services v2 (automatic sign-in, achievements,
 * Saved Games snapshots), the in-app review flow and the licence state.
 *
 * The engine thread calls these through IYIActivity's play* methods; the Play callbacks only write
 * the fields and maps below, so no Java thread ever calls into Python. Every entry point is inert
 * until release.json configures it (the APP_ID meta-data and the resource strings below).
 */
final class PlayServices {
    private static final String TAG = "iyi";
    private static final String APP_ID_META = "com.google.android.gms.games.APP_ID";

    private static volatile boolean initialized, configured, signedIn;
    private static volatile Map<String, String> achievements = Collections.emptyMap();
    // name -> {status, byte[] data, long[] modified_ms}; written by the Play callbacks, read by JNI.
    private static final Map<String, Object[]> cloud = new ConcurrentHashMap<>();
    static volatile String license = "off";   // "off", "pending", "ok" or "denied"
    private static volatile String[] names;   // Saved Games snapshot names; null until listed after sign-in

    private PlayServices() {}

    static void init(Activity activity) {
        if (initialized) return;
        initialized = true;
        achievements = achievements(activity);
        configured = metaData(activity, APP_ID_META) != null;
        if (configured) {
            PlayGamesSdk.initialize(activity);
            refresh(activity);
        }
    }

    /** On resume: Play Games can sign in (or drop) between launches, so re-read the cached state. */
    static void refresh(Activity activity) {
        if (!configured) return;
        PlayGames.getGamesSignInClient(activity).isAuthenticated()
            .addOnCompleteListener(task -> {
                signedIn = task.isSuccessful() && task.getResult().isAuthenticated();
                if (signedIn) loadNames(activity);
            });
    }

    // A fresh install has no local saves to ask about: list the cloud ones. A failed list stays
    // unknown (null, the menu keeps waiting) and is asked again in 10 s, not taken as "none".
    private static void loadNames(Activity activity) {
        PlayGames.getSnapshotsClient(activity).load(true).addOnCompleteListener(task -> {
            SnapshotMetadataBuffer buffer = task.isSuccessful() ? task.getResult().get() : null;
            if (buffer == null) {
                new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> { if (signedIn) loadNames(activity); }, 10000);
                return;
            }
            String[] found = new String[buffer.getCount()];
            for (int i = 0; i < found.length; i++) found[i] = buffer.get(i).getUniqueName();
            buffer.release();
            names = found;
        });
    }

    static String[] cloudNames() {
        return signedIn ? names : null;
    }

    static boolean available() {
        return configured && signedIn;
    }

    static void unlock(Activity activity, String name) {
        String id = achievements.get(name);
        if (id == null || !signedIn) return;
        try {
            PlayGames.getAchievementsClient(activity).unlock(id);
        } catch (Exception e) {
            Log.w(TAG, "play unlock failed: " + e);
        }
    }

    // Play stamps its own commit time, always later than the local file: keep the local mtime in progressValue.
    static void cloudWrite(Activity activity, String name, byte[] data, long modifiedMs) {
        if (!signedIn) return;
        SnapshotsClient snapshots = PlayGames.getSnapshotsClient(activity);
        snapshots.open(name, true, SnapshotsClient.RESOLUTION_POLICY_MOST_RECENTLY_MODIFIED)
            .addOnCompleteListener(task -> {
                Snapshot snapshot = task.isSuccessful() ? task.getResult().getData() : null;
                if (snapshot == null) return;
                try {
                    snapshot.getSnapshotContents().writeBytes(data);
                } catch (Exception e) {
                    Log.w(TAG, "play cloud write failed: " + e);
                    snapshots.discardAndClose(snapshot);
                    return;
                }
                snapshots.commitAndClose(snapshot, new SnapshotMetadataChange.Builder().setProgressValue(modifiedMs).build());
            });
    }

    static void cloudFetch(Activity activity, String name) {
        if (!signedIn) {
            cloud.put(name, result("off", null, 0));
            return;
        }
        cloud.put(name, result("pending", null, 0));
        PlayGames.getSnapshotsClient(activity)
            .open(name, false, SnapshotsClient.RESOLUTION_POLICY_MOST_RECENTLY_MODIFIED)
            .addOnCompleteListener(task -> {
                Snapshot snapshot = task.isSuccessful() ? task.getResult().getData() : null;
                if (snapshot == null) {
                    cloud.put(name, result(missing(task) ? "missing" : "error", null, 0));
                    return;
                }
                try {
                    SnapshotContents contents = snapshot.getSnapshotContents();
                    byte[] data = contents.readFully();
                    long stamp = snapshot.getMetadata().getProgressValue();   // our local mtime; -1/0 = written elsewhere
                    cloud.put(name, result("ready", data == null ? new byte[0] : data,
                                           stamp > 0 ? stamp : snapshot.getMetadata().getLastModifiedTimestamp()));
                } catch (Exception e) {
                    Log.w(TAG, "play cloud fetch failed: " + e);
                    cloud.put(name, result("error", null, 0));
                } finally {
                    PlayGames.getSnapshotsClient(activity).discardAndClose(snapshot);
                }
            });
    }

    static Object[] cloudResult(String name) {
        Object[] entry = cloud.get(name);
        return entry != null ? entry : result("off", null, 0);
    }

    static void requestReview(Activity activity) {
        activity.runOnUiThread(() -> {
            ReviewManager manager = ReviewManagerFactory.create(activity);
            manager.requestReviewFlow().addOnCompleteListener(task -> {
                if (task.isSuccessful())
                    manager.launchReviewFlow(activity, task.getResult()).addOnCompleteListener(launched -> { });
            });
        });
    }

    static String licenseState() {
        return license;
    }

    private static Object[] result(String status, byte[] data, long modifiedMs) {
        return new Object[] { status, data, new long[] { modifiedMs } };
    }

    private static boolean missing(Task<?> task) {
        Exception e = task.getException();
        return e instanceof ApiException
            && ((ApiException) e).getStatusCode() == GamesClientStatusCodes.SNAPSHOT_NOT_FOUND;
    }

    // release.json play_achievement_ids: "NAME=playId;NAME=playId" (tools/release.py encodes it).
    private static Map<String, String> achievements(Activity activity) {
        Map<String, String> map = new HashMap<>();
        for (String pair : string(activity, R.string.iyi_play_achievement_ids).split(";")) {
            int split = pair.indexOf('=');
            if (split > 0 && split < pair.length() - 1)
                map.put(pair.substring(0, split), pair.substring(split + 1));
        }
        return map;
    }

    private static String string(Activity activity, int resource) {
        return activity.getString(resource).trim();
    }

    // Play Games only works when the APP_ID meta-data exists (the release manifest adds it only when
    // release.json play_games_app_id is set); without it every call stays off.
    private static String metaData(Activity activity, String key) {
        try {
            ApplicationInfo info = activity.getPackageManager()
                .getApplicationInfo(activity.getPackageName(), PackageManager.GET_META_DATA);
            Bundle data = info.metaData;
            String value = data == null ? null : data.getString(key);
            return value == null || value.isEmpty() ? null : value;
        } catch (PackageManager.NameNotFoundException e) {
            return null;
        }
    }
}
