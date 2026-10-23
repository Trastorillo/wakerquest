package io.github.wakerquest;

import android.content.Context;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;

// The disc in files/<ID>.iso, checked as native/src/pc/runtime/pc_disc.cpp's checkDisc does (the
// header: game ID at 0x00, revision at 0x07, GameCube magic at 0x1C), plus the compressed formats a
// player is likely to have (CISO, RVZ/WIA, GCZ, NKit) and the size of a full image. The SHA-1 checks
// of the game's files stay on the native side; its "[cos] DISC:" reason reaches DiscSetupActivity.
final class DiscCheck {
    static final long FULL_SIZE = 1459978240L;
    static final int HEADER_SIZE = 0x220;

    private DiscCheck() {
    }

    static String gameId(Context context) {
        return context.getString(R.string.game_id);
    }

    static File discFile(Context context) {
        return new File(context.getExternalFilesDir(null), gameId(context) + ".iso");
    }

    static boolean spanish() {
        return "es".equals(Locale.getDefault().getLanguage());
    }

    static String t(String es, String en) {
        return spanish() ? es : en;
    }

    // null when files/<ID>.iso is there and its header is the right disc's.
    static String installedProblem(Context context) {
        File disc = discFile(context);
        if (!disc.isFile()) {
            return t("No encuentro el disco de Wind Waker. Elige tu ISO (por ejemplo en Descargas).",
                     "The Wind Waker disc is missing. Choose your ISO (for example in Downloads).");
        }
        try (InputStream in = new FileInputStream(disc)) {
            return problem(context, readHeader(in), disc.length());
        } catch (IOException e) {
            return t("No puedo leer " + disc.getName() + ": " + e.getMessage(),
                     "Cannot read " + disc.getName() + ": " + e.getMessage());
        }
    }

    // Up to HEADER_SIZE bytes (fewer for a short file).
    static byte[] readHeader(InputStream in) throws IOException {
        byte[] buf = new byte[HEADER_SIZE];
        int got = 0;
        while (got < buf.length) {
            int n = in.read(buf, got, buf.length - got);
            if (n < 0) {
                break;
            }
            got += n;
        }
        if (got == buf.length) {
            return buf;
        }
        byte[] shorter = new byte[got];
        System.arraycopy(buf, 0, shorter, 0, got);
        return shorter;
    }

    // null if the image is this build's disc, revision 0, uncompressed; otherwise the player's message.
    // size < 0: unknown.
    static String problem(Context context, byte[] h, long size) {
        String want = gameId(context);
        String wantName = regionName(want);
        if (h.length < 0x20) {
            return t("El archivo es demasiado pequeño para ser una ISO.", "The file is too small to be an ISO.");
        }
        String compressed = null;
        if (startsWith(h, "CISO")) {
            compressed = "CISO";
        } else if (startsWith(h, "RVZ\u0001")) {
            compressed = "RVZ";
        } else if (startsWith(h, "WIA\u0001")) {
            compressed = "WIA";
        } else if ((h[0] & 0xFF) == 0x01 && (h[1] & 0xFF) == 0xC0 && (h[2] & 0xFF) == 0x0B
                   && (h[3] & 0xFF) == 0xB1) {
            compressed = "GCZ";
        }
        if (compressed != null) {
            return t("Está comprimida (" + compressed + "); conviértela a ISO con Dolphin (clic derecho en el "
                     + "juego → Convertir archivo → ISO).",
                     "It is compressed (" + compressed + "); convert it to ISO in Dolphin (right-click the game → "
                     + "Convert File → ISO).");
        }
        long magic = ((long)(h[0x1C] & 0xFF) << 24) | ((h[0x1D] & 0xFF) << 16) | ((h[0x1E] & 0xFF) << 8)
                     | (h[0x1F] & 0xFF);
        if (magic != 0xC2339F3DL) {
            return t("No es una imagen de GameCube (¿está comprimida o es de otra consola?).",
                     "This is not a GameCube disc image (compressed, or another console's?).");
        }
        StringBuilder idBuilder = new StringBuilder();
        for (int i = 0; i < 6; i++) {
            int c = h[i] & 0xFF;
            idBuilder.append(c >= 0x20 && c < 0x7F ? (char)c : '?');
        }
        String id = idBuilder.toString();
        if (!id.equals(want)) {
            if (id.startsWith("GZL")) {
                return t("Esta ISO es la " + regionName(id) + " (" + id + "); este APK es para la " + wantName
                         + " (" + want + ").",
                         "This ISO is the " + regionNameEn(id) + " disc (" + id + "); this APK is for the "
                         + regionNameEn(want) + " one (" + want + ").");
            }
            return t("Esta ISO es de otro juego (" + id + "); hace falta la ISO " + wantName + " de The Wind Waker ("
                     + want + ").",
                     "This ISO is another game (" + id + "); it needs The Wind Waker, " + regionNameEn(want)
                     + " disc (" + want + ").");
        }
        int revision = h[7] & 0xFF;
        if (revision != 0) {
            return t("Esta ISO es la revisión " + revision + "; hace falta la revisión 0.",
                     "This ISO is revision " + revision + "; it needs revision 0.");
        }
        if (h.length >= 0x204 && h[0x200] == 'N' && h[0x201] == 'K' && h[0x202] == 'I' && h[0x203] == 'T') {
            return t("Es una imagen NKit (recortada); recupera la ISO completa con NKit Processing App.",
                     "This is an NKit (trimmed) image; restore the full ISO with NKit Processing App.");
        }
        if (size >= 0 && size != FULL_SIZE) {
            return t("Mide " + size / (1024 * 1024) + " MB y una ISO completa mide 1392 MB: está recortada o "
                     + "incompleta.",
                     "It is " + size / (1024 * 1024) + " MB and a full ISO is 1392 MB: it is trimmed or "
                     + "incomplete.");
        }
        return null;
    }

    private static boolean startsWith(byte[] h, String s) {
        for (int i = 0; i < s.length(); i++) {
            if (h[i] != (byte)s.charAt(i)) {
                return false;
            }
        }
        return true;
    }

    private static String regionName(String id) {
        switch (id.length() > 3 ? id.charAt(3) : '?') {
            case 'E': return "americana";
            case 'P': return "europea";
            case 'J': return "japonesa";
            default: return "de otra región";
        }
    }

    private static String regionNameEn(String id) {
        switch (id.length() > 3 ? id.charAt(3) : '?') {
            case 'E': return "USA";
            case 'P': return "European";
            case 'J': return "Japanese";
            default: return "other region's";
        }
    }
}
