package io.github.wakerquest;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

// The disc's setup, a 2D panel in its own process (":setup", so the game's process and SDL's static
// state start fresh afterwards): WakerQuestActivity opens it when files/<ID>.iso is missing or wrong,
// and the native side when its disc check fails (EXTRA_REASON, the "[cos] DISC:" line). The player
// picks the ISO with the system's file picker (no storage permission), it is checked, copied to
// files/<ID>.iso.part and renamed; the source stays where it was.
public class DiscSetupActivity extends Activity {
    static final String EXTRA_REASON = "reason";
    private static final int PICK_REQUEST_CODE = 0x5751;
    private static final String EXTERNAL_STORAGE_AUTHORITY = "com.android.externalstorage.documents";

    private TextView message;
    private ProgressBar progress;
    private Button pickButton;
    private Button foundButton;
    private Button playButton;
    private File found;
    private volatile boolean copying;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pad = (int)(32 * getResources().getDisplayMetrics().density);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("WakerQuest");
        title.setTextSize(28);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        message = new TextView(this);
        message.setTextSize(18);
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, pad / 2, 0, pad / 2);
        layout.addView(message);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        layout.addView(progress, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                                               LinearLayout.LayoutParams.WRAP_CONTENT));

        foundButton = addButton(layout, "", v -> copyFile(found));
        pickButton = addButton(layout, DiscCheck.t("Elegir ISO", "Choose ISO"), v -> pick());
        playButton = addButton(layout, DiscCheck.t("Jugar", "Play"), v -> play());
        setContentView(layout);

        String reason = getIntent().getStringExtra(EXTRA_REASON);
        if (reason != null) {
            showProblem(DiscCheck.t("El juego no ha podido usar el disco:\n", "The game could not use the disc:\n")
                        + reason);
        } else {
            refresh();
        }
    }

    private Button addButton(LinearLayout layout, String text, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(text);
        button.setTextSize(18);
        button.setOnClickListener(listener);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                                                                         LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = (int)(12 * getResources().getDisplayMetrics().density);
        layout.addView(button, params);
        return button;
    }

    // The installed disc's state: ready (play), or its problem plus the picker and any ISO found.
    private void refresh() {
        String problem = DiscCheck.installedProblem(this);
        if (problem == null) {
            message.setText(DiscCheck.t("Disco listo.", "Disc ready."));
            pickButton.setVisibility(View.GONE);
            foundButton.setVisibility(View.GONE);
            playButton.setVisibility(View.VISIBLE);
            return;
        }
        showProblem(problem);
    }

    private void showProblem(String text) {
        message.setText(text);
        progress.setVisibility(View.GONE);
        pickButton.setVisibility(View.VISIBLE);
        pickButton.setEnabled(true);
        // Retry only makes sense with a disc that looks right (the native side may say otherwise).
        playButton.setVisibility(DiscCheck.installedProblem(this) == null ? View.VISIBLE : View.GONE);
        found = findIso();
        if (found != null) {
            foundButton.setText(DiscCheck.t("¿Usar este archivo? ", "Use this file? ") + found.getAbsolutePath());
            foundButton.setVisibility(View.VISIBLE);
            foundButton.setEnabled(true);
        } else {
            foundButton.setVisibility(View.GONE);
        }
    }

    // A right ISO in Download/ or the storage's root, when the app can read them without a permission
    // (often it cannot: then only the picker).
    private File findIso() {
        File root = Environment.getExternalStorageDirectory();
        File[] dirs = { new File(root, Environment.DIRECTORY_DOWNLOADS), root };
        for (File dir : dirs) {
            File[] files = dir.listFiles();
            if (files == null) {
                continue;
            }
            for (File f : files) {
                String name = f.getName().toLowerCase();
                if (!f.isFile() || !f.canRead() || !(name.endsWith(".iso") || name.endsWith(".gcm"))) {
                    continue;
                }
                try (InputStream in = new FileInputStream(f)) {
                    if (DiscCheck.problem(this, DiscCheck.readHeader(in), f.length()) == null) {
                        return f;
                    }
                } catch (IOException ignored) {
                }
            }
        }
        return null;
    }

    private void pick() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI,
                        DocumentsContract.buildDocumentUri(EXTERNAL_STORAGE_AUTHORITY, "primary:Download"));
        try {
            startActivityForResult(intent, PICK_REQUEST_CODE);
        } catch (Exception e) {
            message.setText(DiscCheck.t("No puedo abrir el selector de archivos: ", "Cannot open the file picker: ")
                            + e.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == PICK_REQUEST_CODE && resultCode == RESULT_OK && data != null && data.getData() != null) {
            copyUri(data.getData());
        }
    }

    private void copyUri(Uri uri) {
        long size = -1;
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.SIZE }, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) {
                size = c.getLong(0);
            }
        } catch (Exception ignored) {
        }
        final long knownSize = size;
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            String problem = DiscCheck.problem(this, DiscCheck.readHeader(in), knownSize);
            if (problem != null) {
                showProblem(problem);
                return;
            }
        } catch (Exception e) {
            showProblem(DiscCheck.t("No puedo leer el archivo: ", "Cannot read the file: ") + e.getMessage());
            return;
        }
        startCopy(() -> getContentResolver().openInputStream(uri), knownSize);
    }

    private void copyFile(File source) {
        if (source != null) {
            startCopy(() -> new FileInputStream(source), source.length());
        }
    }

    private interface Source {
        InputStream open() throws IOException;
    }

    // Copies to <ID>.iso.part on a thread, then checks the copy's header and size and renames it.
    private void startCopy(Source source, long size) {
        if (copying) {
            return;
        }
        File dest = DiscCheck.discFile(this);
        File part = new File(dest.getPath() + ".part");
        long need = size > 0 ? size : DiscCheck.FULL_SIZE;
        File dir = dest.getParentFile();
        if (dir != null && dir.getUsableSpace() + (dest.isFile() ? dest.length() : 0) < need) {
            showProblem(DiscCheck.t("No hay espacio: hacen falta " + need / (1024 * 1024) + " MB libres.",
                                    "Not enough space: " + need / (1024 * 1024) + " MB free are needed."));
            return;
        }
        copying = true;
        message.setText(DiscCheck.t("Copiando el disco…", "Copying the disc…"));
        progress.setProgress(0);
        progress.setVisibility(View.VISIBLE);
        pickButton.setEnabled(false);
        foundButton.setEnabled(false);
        playButton.setVisibility(View.GONE);
        new Thread(() -> {
            String error = null;
            try (InputStream in = source.open(); OutputStream out = new FileOutputStream(part)) {
                byte[] buf = new byte[1 << 20];
                long done = 0;
                int lastShown = -1;
                for (int n; (n = in.read(buf)) > 0;) {
                    out.write(buf, 0, n);
                    done += n;
                    int shown = (int)(done * 1000 / need);
                    if (shown != lastShown) {
                        lastShown = shown;
                        runOnUiThread(() -> progress.setProgress(shown));
                    }
                }
                out.flush();
                ((FileOutputStream)out).getFD().sync();
            } catch (IOException e) {
                error = e.getMessage();
            }
            if (error == null) {
                try (InputStream in = new FileInputStream(part)) {
                    error = DiscCheck.problem(this, DiscCheck.readHeader(in), part.length());
                } catch (IOException e) {
                    error = e.getMessage();
                }
            }
            // rename(2) replaces the old disc at once; only if it cannot is the old one deleted first
            if (error == null && !part.renameTo(dest)) {
                if (dest.exists() && !dest.delete()) {
                    error = DiscCheck.t("No puedo reemplazar ", "Cannot replace ") + dest.getName();
                } else if (!part.renameTo(dest)) {
                    error = DiscCheck.t("No puedo renombrar ", "Cannot rename ") + part.getName();
                }
            }
            if (error != null) {
                part.delete();
            }
            final String result = error;
            runOnUiThread(() -> {
                copying = false;
                if (result == null) {
                    play();
                } else {
                    showProblem(DiscCheck.t("La copia ha fallado: ", "The copy failed: ") + result);
                }
            });
        }, "DiscCopy").start();
    }

    // Back to the game: the old log (with its "[cos] DISC:" line) is set aside so it does not bring
    // this panel back.
    private void play() {
        File log = new File(getExternalFilesDir(null), "logs/switchwaker.log");
        if (log.isFile()) {
            log.renameTo(new File(log.getPath() + ".disc"));
        }
        Intent intent = new Intent(this, WakerQuestActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
        finish();
    }

    @Override
    public void onBackPressed() {
        if (!copying) {
            super.onBackPressed();
        }
    }
}
