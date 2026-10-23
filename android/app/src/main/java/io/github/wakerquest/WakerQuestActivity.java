package io.github.wakerquest;

import android.content.Intent;
import android.os.Bundle;

import dev.encounter.borealis.BorealisActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

// SDL's activity through Borealis (immersive mode, surface lifecycle, launch arguments): it loads
// libmain.so and runs the game's SDL_main. The data (<ID>.iso, user/, logs/) lives in
// getExternalFilesDir(null), which the native side asks SDL for (pc_harness.cpp).
// Without a usable disc the game does not start: DiscSetupActivity (a 2D panel) sets it up, and this
// process ends before SDL's thread starts (SDLActivity allows one activity per process).
public class WakerQuestActivity extends BorealisActivity {
    private boolean toDiscSetup;

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
