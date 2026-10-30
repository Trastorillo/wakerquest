package io.github.wakerquest;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;

import dev.encounter.borealis.BorealisActivity;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

// SDL's activity through Borealis (immersive mode, surface lifecycle, launch arguments): it loads
// libmain.so and runs the game's SDL_main. The data (<ID>.iso, user/, logs/) lives in
// getExternalFilesDir(null), which the native side asks SDL for (pc_harness.cpp).
// Without a usable disc the game does not start: DiscSetupActivity (a 2D panel) sets it up, and this
// process ends before SDL's thread starts (SDLActivity allows one activity per process).
public class WakerQuestActivity extends BorealisActivity {
    private boolean toDiscSetup;

    // The settings panel's update check (android/xr/wq_xr.cpp, hostInfo and hostCheckUpdate; fase 2): the
    // latest version published on GitHub, compared with this APK's. It only reads; nothing is downloaded.
    // The newest release's tag, else the newest tag, else the version in the quest branch's CMakeLists.
    private static final String TAG = "WakerQuest";
    private static final String REPO = "Trastorillo/wakerquest";
    private static final String RELEASE_URL = "https://api.github.com/repos/" + REPO + "/releases/latest";
    private static final String TAGS_URL = "https://api.github.com/repos/" + REPO + "/tags?per_page=30";
    private static final String CMAKE_URL = "https://raw.githubusercontent.com/" + REPO + "/quest/android/CMakeLists.txt";
    // "" none yet, "busy", "latest\t<local>\t<published>", "new\t<published>\t<local>", "error\t<why>"
    private volatile String updateStatus = "";
    private volatile boolean updateRunning;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        boolean discMissing = DiscCheck.installedProblem(this) != null;
        String nativeReason = discMissing ? null : lastDiscError();
        super.onCreate(savedInstanceState);
        if (discMissing || nativeReason != null) {
            toDiscSetup = true;
            openDiscSetup(nativeReason);
            finish();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (toDiscSetup) {
            System.exit(0);
        }
    }

    private void openDiscSetup(String nativeReason) {
        Intent intent = new Intent(this, DiscSetupActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (nativeReason != null) {
            intent.putExtra(DiscSetupActivity.EXTRA_REASON, nativeReason);
        }
        startActivity(intent);
    }

    // Called by the native side (pc_disc.cpp, exitDisc) on the game's thread just before it exits.
    public void showDiscError(String reason) {
        openDiscSetup(reason);
    }

    // This APK's versionName (the settings panel's Acerca de), "?" if the package manager does not say.
    public String versionName() {
        try {
            String v = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
            return v != null ? v : "?";
        } catch (PackageManager.NameNotFoundException e) {
            return "?";
        }
    }

    // The update check's state, for the settings panel (polled from the headset layer's thread).
    public String updateStatus() {
        return updateStatus;
    }

    // Starts the update check on a thread of its own (never the caller's: the headset layer's frames);
    // one at a time.
    public synchronized void checkForUpdate() {
        if (updateRunning) {
            return;
        }
        updateRunning = true;
        updateStatus = "busy";
        Thread t = new Thread(() -> {
            String status = "error\t?";
            try {
                String local = versionName();
                String published = latestPublished();
                status = compareVersions(published, local) > 0 ? "new\t" + published + "\t" + local
                                                                : "latest\t" + local + "\t" + published;
            } catch (UnknownHostException e) {
                status = "error\toffline";
            } catch (Throwable e) {
                // short, to fit the panel's line ("HTTP 403", the exception's name)
                String why = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
                status = "error\t" + (why.length() > 32 ? why.substring(0, 32) : why);
            } finally {
                Log.i(TAG, "update check: " + status.replace('\t', ' '));
                updateStatus = status;
                updateRunning = false;
            }
        }, "wq-update-check");
        t.setDaemon(true);
        t.start();
    }

    // The latest version published (no leading "v"): the newest release, else the highest tag, else the
    // quest branch's COS_ANDROID_VERSION_NAME. Throws the first source's error when none answers.
    private static String latestPublished() throws Exception {
        Exception first = null;
        try {
            String tag = new JSONObject(get(RELEASE_URL)).getString("tag_name");
            if (!tag.isEmpty()) {
                return stripV(tag);
            }
        } catch (Exception e) {
            first = e;
        }
        try {
            JSONArray tags = new JSONArray(get(TAGS_URL));
            String best = null;
            for (int i = 0; i < tags.length(); i++) {
                String name = stripV(tags.getJSONObject(i).getString("name"));
                if (!name.isEmpty() && Character.isDigit(name.charAt(0)) && (best == null || compareVersions(name, best) > 0)) {
                    best = name;
                }
            }
            if (best != null) {
                return best;
            }
        } catch (Exception e) {
            if (first == null) {
                first = e;
            }
        }
        try {
            Matcher m = Pattern.compile("COS_ANDROID_VERSION_NAME \"([^\"]+)\"").matcher(get(CMAKE_URL));
            if (m.find()) {
                return m.group(1);
            }
        } catch (Exception e) {
            if (first == null) {
                first = e;
            }
        }
        throw first != null ? first : new IOException("no version found");
    }

    private static String get(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        try {
            c.setConnectTimeout(8000);
            c.setReadTimeout(8000);
            c.setRequestProperty("Accept", "application/vnd.github+json");
            c.setRequestProperty("User-Agent", "WakerQuest-update-check");
            int code = c.getResponseCode();
            if (code != 200) {
                throw new IOException("HTTP " + code);
            }
            try (InputStream in = c.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                for (int n; (n = in.read(buf)) > 0;) {
                    out.write(buf, 0, n);
                }
                return new String(out.toByteArray(), StandardCharsets.UTF_8);
            }
        } finally {
            c.disconnect();
        }
    }

    private static String stripV(String v) {
        v = v.trim();
        return v.startsWith("v") || v.startsWith("V") ? v.substring(1) : v;
    }

    // Versions by their numbers: 1.0.9 > 1.0.8-fase1 > 1.0.8 > 1.0.7, 1.0.6.1 > 1.0.6. The dotted numbers
    // first (missing ones are 0); then, with the same numbers, a suffix's own numbers (fase2 > fase1), and
    // a suffix over none (the phases of a version are built on it).
    static int compareVersions(String a, String b) {
        String[] pa = splitVersion(stripV(a)), pb = splitVersion(stripV(b));
        int c = compareNumbers(numbers(pa[0]), numbers(pb[0]));
        if (c != 0) {
            return c;
        }
        if (pa[1].isEmpty() != pb[1].isEmpty()) {
            return pa[1].isEmpty() ? -1 : 1;
        }
        return compareNumbers(numbers(pa[1]), numbers(pb[1]));
    }

    // "1.0.8-fase1" -> {"1.0.8", "fase1"}
    private static String[] splitVersion(String v) {
        int i = 0;
        while (i < v.length() && (Character.isDigit(v.charAt(i)) || v.charAt(i) == '.')) {
            i++;
        }
        String rest = v.substring(i);
        while (!rest.isEmpty() && !Character.isLetterOrDigit(rest.charAt(0))) {
            rest = rest.substring(1);
        }
        return new String[] {v.substring(0, i), rest};
    }

    private static List<Long> numbers(String s) {
        List<Long> out = new ArrayList<>();
        Matcher m = Pattern.compile("\\d+").matcher(s);
        while (m.find()) {
            try {
                out.add(Long.parseLong(m.group()));
            } catch (NumberFormatException e) {
                out.add(Long.MAX_VALUE);
            }
        }
        return out;
    }

    private static int compareNumbers(List<Long> a, List<Long> b) {
        for (int i = 0; i < Math.max(a.size(), b.size()); i++) {
            long x = i < a.size() ? a.get(i) : 0, y = i < b.size() ? b.get(i) : 0;
            if (x != y) {
                return x < y ? -1 : 1;
            }
        }
        return 0;
    }

    // The reason of the last run's disc failure ("[cos] DISC: ..." near the top of the log), if the
    // native side could not open the panel itself.
    private String lastDiscError() {
        File log = new File(getExternalFilesDir(null), "logs/switchwaker.log");
        if (!log.isFile()) {
            return null;
        }
        byte[] buf = new byte[64 * 1024];
        int got = 0;
        try (InputStream in = new FileInputStream(log)) {
            for (int n; got < buf.length && (n = in.read(buf, got, buf.length - got)) > 0;) {
                got += n;
            }
        } catch (IOException e) {
            return null;
        }
        String text = new String(buf, 0, got, StandardCharsets.UTF_8);
        String key = "[cos] DISC: ";
        int at = text.indexOf(key);
        if (at < 0) {
            return null;
        }
        int end = text.indexOf('\n', at);
        return text.substring(at + key.length(), end < 0 ? text.length() : end);
    }
}
