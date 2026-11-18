package io.github.wakerquest;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.util.Arrays;
import java.util.Locale;

// The disc in files/<ID>.iso, checked as native/src/pc/runtime/pc_disc.cpp's checkDisc does (the
// header: game ID at 0x00, revision at 0x07, GameCube magic at 0x1C), plus the compressed formats a
// player is likely to have (RVZ/WIA, GCZ) and whether fst.bin's files lie within the image. The SHA-1
// checks of the game's files stay on the native side; its "[cos] DISC:" reason reaches DiscSetupActivity.
// 1.5: a GameCube CISO is read through its block map (as switch/native/nod/nod_gcn.cpp reads it), and an
// NKit image (shorter, its padding removed and fst.bin rewritten) is a plain one: no size is required.
final class DiscCheck {
    static final long FULL_SIZE = 1459978240L;
    static final int HEADER_SIZE = 0x220;
    // CISO: "CISO", the block size (little-endian) at 4, one byte per block from 8 (1 = stored), the stored
    // blocks one after another from CISO_HEADER
    static final int CISO_HEADER = 0x8000;
    static final int CISO_MAP_SIZE = CISO_HEADER - 8;
    // what readHeader reads: a CISO's header and its disc's first bytes
    static final int READ_SIZE = CISO_HEADER + HEADER_SIZE;
    private static final String TAG = "WakerQuest";

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

    // null when files/<ID>.iso is there, its header is the right disc's and its files are in it.
    static String installedProblem(Context context) {
        File disc = discFile(context);
        if (!disc.isFile()) {
            return t("No encuentro el disco de Wind Waker. Elige tu ISO (por ejemplo en Descargas).",
                     "The Wind Waker disc is missing. Choose your ISO (for example in Downloads).");
        }
        try (InputStream in = new FileInputStream(disc)) {
            String problem = problem(context, readHeader(in), disc.length());
            return problem != null ? problem : fstProblem(disc);
        } catch (IOException e) {
            return t("No puedo leer " + disc.getName() + ": " + e.getMessage(),
                     "Cannot read " + disc.getName() + ": " + e.getMessage());
        }
    }

    // Up to READ_SIZE bytes (fewer for a short file).
    static byte[] readHeader(InputStream in) throws IOException {
        byte[] buf = new byte[READ_SIZE];
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
        return Arrays.copyOf(buf, got);
    }

    // A GameCube CISO's block size; 0 if the file is not a CISO, -1 if it is one this cannot read (PSP's CSO,
    // which has its header's size, 0x18, at 4, or a damaged one).
    static int cisoBlockSize(byte[] h) {
        if (h.length < 8 || !startsWith(h, "CISO")) {
            return 0;
        }
        int block = (h[4] & 0xFF) | (h[5] & 0xFF) << 8 | (h[6] & 0xFF) << 16 | (h[7] & 0xFF) << 24;
        return block >= 0x800 && (block & (block - 1)) == 0 ? block : -1;
    }

    // Where a CISO keeps the disc's byte at offset: its place in the file, or -1 where the block is not
    // stored (zeros). map: the file's first CISO_HEADER bytes.
    static long cisoFileOffset(byte[] map, int block, long offset) {
        long index = offset / block;
        if (index >= CISO_MAP_SIZE || map[8 + (int)index] != 1) {
            return -1;
        }
        long stored = 0;
        for (int i = 0; i < index; i++) {
            if (map[8 + i] == 1) {
                stored++;
            }
        }
        return CISO_HEADER + stored * block + offset % block;
    }

    // The disc's size in a CISO: its blocks up to the last stored one, at least a full disc (nod_gcn.cpp).
    static long cisoDiscSize(byte[] map, int block) {
        int blocks = 0;
        for (int i = 0; i < CISO_MAP_SIZE; i++) {
            if (map[8 + i] == 1) {
                blocks = i + 1;
            }
        }
        return Math.max((long)blocks * block, Math.min(FULL_SIZE, (long)CISO_MAP_SIZE * block));
    }

    // null if the image is this build's disc, revision 0, a plain image (ISO, NKit) or a GameCube CISO;
    // otherwise the player's message. size < 0: unknown.
    static String problem(Context context, byte[] h, long size) {
        String want = gameId(context);
        String wantName = regionName(want);
        if (h.length < 0x20) {
            return t("El archivo es demasiado pequeño para ser una ISO.", "The file is too small to be an ISO.");
        }
        String format = "iso";
        int ciso = cisoBlockSize(h);
        if (ciso < 0) {
            return t("Es una CISO que no puedo leer (¿de PSP o dañada?); conviértela a ISO con Dolphin (clic "
                     + "derecho en el juego → Convertir archivo → ISO).",
                     "It is a CISO this cannot read (PSP's, or damaged?); convert it to ISO in Dolphin (right-click "
                     + "the game → Convert File → ISO).");
        }
        if (ciso > 0) {
            // the disc's first block is the first stored one, at CISO_HEADER (a block holds HEADER_SIZE)
            if (h.length < READ_SIZE || h[8] != 1) {
                return t("Es una CISO sin la cabecera del disco: está dañada o incompleta.",
                         "It is a CISO without the disc's header: damaged or incomplete.");
            }
            h = Arrays.copyOfRange(h, CISO_HEADER, READ_SIZE);
            format = "ciso";
        }
        String compressed = null;
        if (startsWith(h, "RVZ\u0001")) {
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
        boolean nkit = h.length >= 0x204 && h[0x200] == 'N' && h[0x201] == 'K' && h[0x202] == 'I' && h[0x203] == 'T';
        if (nkit && ciso == 0) {
            format = "nkit";
        }
        // the size only for the log: an NKit image is shorter, a CISO has no empty blocks; whether the files
        // are in it is fstProblem's (once the image is a file of its own)
        Log.i(TAG, "disc: " + id + " revision 0, " + format + (nkit && ciso > 0 ? " (nkit)" : "") + ", "
                   + size + " bytes" + (ciso == 0 && !nkit && size >= 0 && size != FULL_SIZE
                                        ? " (a full ISO is " + FULL_SIZE + ")" : ""));
        return null;
    }

    // null if main.dol, fst.bin and every file fst.bin names lie within the image (read through a CISO's
    // block map); otherwise the player's message: the image is cut short (an incomplete download or copy).
    static String fstProblem(File file) {
        String cut = t("La imagen está incompleta (le faltan datos del final): vuelve a copiarla o descargarla.",
                       "The image is incomplete (data missing at its end): copy or download it again.");
        try (RandomAccessFile f = new RandomAccessFile(file, "r")) {
            byte[] map = new byte[CISO_HEADER];
            int block = f.length() >= CISO_HEADER ? cisoBlockSize(readFully(f, 0, map)) : 0;
            long size = f.length();
            if (block > 0) {
                size = cisoDiscSize(map, block);
                long stored = 0;
                for (int i = 0; i < CISO_MAP_SIZE; i++) {
                    if (map[8 + i] == 1) {
                        stored++;
                    }
                }
                // the last stored block may be cut short in the file (some tools do not pad it)
                if (stored > 0 && CISO_HEADER + (stored - 1) * block >= f.length()) {
                    return cut;
                }
            } else {
                block = 0;
            }
            byte[] boot = readDisc(f, map, block, 0, 0x440);
            long dol = be32(boot, 0x420), fstOffset = be32(boot, 0x424), fstSize = be32(boot, 0x428);
            if (dol >= size || fstOffset + fstSize > size || fstSize < 12 || fstSize > (16 << 20)) {
                return cut;
            }
            byte[] fst = readDisc(f, map, block, fstOffset, (int)fstSize);
            long count = be32(fst, 8);
            if (count * 12 > fstSize) {
                return t("El fst.bin del disco está dañado.", "The disc's fst.bin is damaged.");
            }
            for (int i = 1; i < count; i++) {
                if (fst[12 * i] != 0) {
                    continue;  // a directory
                }
                if (be32(fst, 12 * i + 4) + be32(fst, 12 * i + 8) > size) {
                    return cut;
                }
            }
            return null;
        } catch (IOException e) {
            return t("No puedo leer " + file.getName() + ": " + e.getMessage(),
                     "Cannot read " + file.getName() + ": " + e.getMessage());
        }
    }

    // len bytes of the disc at offset: the file's own, or through a CISO's block map (block > 0).
    private static byte[] readDisc(RandomAccessFile f, byte[] map, int block, long offset, int len)
        throws IOException {
        byte[] out = new byte[len];
        if (block == 0) {
            f.seek(offset);
            f.readFully(out);
            return out;
        }
        int done = 0;
        while (done < len) {
            long at = offset + done;
            int n = (int)Math.min(len - done, block - at % block);
            long from = cisoFileOffset(map, block, at);
            if (from >= 0) {
                // zeros past the file's end (a last block cut short)
                int have = (int)Math.max(0, Math.min(n, f.length() - from));
                if (have > 0) {
                    f.seek(from);
                    f.readFully(out, done, have);
                }
            }
            done += n;
        }
        return out;
    }

    private static byte[] readFully(RandomAccessFile f, long offset, byte[] out) throws IOException {
        f.seek(offset);
        f.readFully(out);
        return out;
    }

    private static long be32(byte[] b, int at) {
        return ((long)(b[at] & 0xFF) << 24) | ((b[at + 1] & 0xFF) << 16) | ((b[at + 2] & 0xFF) << 8)
               | (b[at + 3] & 0xFF);
    }

    private static boolean startsWith(byte[] h, String s) {
        if (h.length < s.length()) {
            return false;
        }
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
