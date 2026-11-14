package org.witchtime.android;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.system.Os;
import android.system.OsConstants;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.channels.FileLock;
import java.security.DigestInputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.*;
import java.util.zip.*;

/** Internal, user-approved binaries; format validation does not make native code safe. */
public final class GpuDriverStore {
    public static final String BUNDLED_ID = "turnip-adreno830-unleashed-mesa26.3-gralloc";
    private static final String BUNDLED_ASSET = "bundled_driver/adreno830/",
            BUNDLED_LIBRARY = "vulkan.mesa26_3_gralloc_a8xx.so",
            BUNDLED_HASH = "64ad4fbab81e787eb4373bccc4d02604e1b8a87bd2dc7e850bc9f7e91f881221";
    private static final long BUNDLED_SIZE = 14495432;
    private static final String SESSION = UUID.randomUUID().toString();
    private static final long SOURCE_LIMIT = 64L << 20, FILE_LIMIT = 128L << 20,
            TOTAL_LIMIT = 256L << 20;
    private static boolean frozen;
    private final Context context;
    private final File root;

    public static final class Driver {
        public final String id, name, version;
        private final String library;
        private Driver(String id, String name, String version, String library) {
            this.id = id; this.name = name; this.version = version; this.library = library;
        }
    }

    public GpuDriverStore(Context context) {
        this.context = context;
        root = new File(context.getFilesDir(), "gpu-drivers");
    }

    public List<Driver> listDrivers() {
        synchronized (GpuDriverStore.class) {
            List<Driver> result = new ArrayList<>();
            File[] files = root.listFiles();
            if (files != null) for (File file : files) {
                if (!file.getName().startsWith("pkg-") && !file.getName().equals(BUNDLED_ID)) continue;
                try { result.add(readDriver(file)); } catch (IOException ignored) { }
            }
            result.sort((a, b) -> a.id.compareTo(b.id));
            return result;
        }
    }

    public String selectedId() {
        synchronized (GpuDriverStore.class) {
            try {
                String id = selection()[0];
                return validId(id) ? id : "system";
            } catch (IOException e) { return "system"; }
        }
    }

    public void select(String id) throws IOException {
        synchronized (GpuDriverStore.class) {
            mutable();
            mkdir(root);
            try (RandomAccessFile lock = new RandomAccessFile(new File(root, "state.lock"), "rw");
                 FileLock held = lock.getChannel().lock()) {
                validateSelection(id);
                String consumed = selection()[1];
                atomicWrite(new File(root, "selected"), id + (consumed.isEmpty() ? "" : "\n" + consumed));
                clearQuarantine(id);
            }
        }
    }

    public String recoveryMessage() {
        synchronized (GpuDriverStore.class) {
            return markers(selectedId()).length == 0 ? "" :
                    "Custom driver startup did not complete. System driver will be used until explicit retry.";
        }
    }

    public boolean hasRecoveryPending() {
        synchronized (GpuDriverStore.class) { return markers(selectedId()).length != 0; }
    }

    /** Worker-thread I/O. Publishes only a new immutable package, never selects it. */
    public Driver ensureBundledDriver() throws IOException {
        if (Build.VERSION.SDK_INT < 35) throw new IOException("Bundled profile requires Android API 35");
        File destination = new File(root, BUNDLED_ID);
        if (destination.exists()) return validateBundled(destination);
        File stage;
        synchronized (GpuDriverStore.class) {
            mkdir(root);
            stage = new File(root, ".stage-" + UUID.randomUUID());
            mkdir(stage);
        }
        try {
            for (String name : new String[] { "meta.json", "bundle-manifest.json", BUNDLED_LIBRARY }) {
                try (InputStream in = context.getAssets().open(BUNDLED_ASSET + name);
                     FileOutputStream out = new FileOutputStream(new File(stage, name))) {
                    copy(in, out, name.equals(BUNDLED_LIBRARY) ? BUNDLED_SIZE : 16384, null);
                    out.getFD().sync();
                }
            }
            validateBundled(stage);
            protectPackage(stage);
            synchronized (GpuDriverStore.class) {
                try (RandomAccessFile lock = new RandomAccessFile(new File(root, "state.lock"), "rw");
                     FileLock held = lock.getChannel().lock()) {
                    if (!destination.exists()) {
                        if (!stage.renameTo(destination)) throw new IOException("Cannot publish bundled driver");
                        syncDirectory(root);
                    }
                }
            }
            return validateBundled(destination);
        } finally { removeStage(stage); }
    }

    /** Staging while frozen is allowed; selected bytes and quarantine stay unchanged. */
    public String stageRestart(String driverId, boolean explicitRetry) throws IOException {
        synchronized (GpuDriverStore.class) {
            mkdir(root);
            try (RandomAccessFile lock = new RandomAccessFile(new File(root, "state.lock"), "rw");
                 FileLock held = lock.getChannel().lock()) {
                validateSelection(driverId);
                if (!driverId.equals("system") && markers(driverId).length != 0 && !explicitRetry)
                    throw new IOException("Explicit retry required for quarantined driver");
                File request = new File(root, "restart");
                if (request.exists()) {
                    String[] previous = restartRequest();
                    boolean abandoned = previous[0].equals("STAGED") && !previous[2].equals(SESSION);
                    if (!abandoned && !previous[1].equals(selection()[1]))
                        throw new IOException("A GPU driver restart is already staged");
                    if (!request.delete()) throw new IOException("Cannot retire consumed restart");
                    syncDirectory(root);
                }
                String nonce = UUID.randomUUID().toString();
                atomicWrite(request, "STAGED\n" + nonce + "\n" + SESSION + "\n" + driverId + "\n" + explicitRetry);
                return nonce;
            }
        }
    }

    public void sealRestart(String nonce) throws IOException {
        synchronized (GpuDriverStore.class) {
            mkdir(root);
            try (RandomAccessFile lock = new RandomAccessFile(new File(root, "state.lock"), "rw");
                 FileLock held = lock.getChannel().lock()) {
                String[] request = restartRequest();
                if (!matches(request, nonce) || !request[0].equals("STAGED") || !request[2].equals(SESSION))
                    throw new IOException("Stale GPU driver restart token");
                request[0] = "SEALED";
                atomicWrite(new File(root, "restart"), String.join("\n", request));
            }
        }
    }

    public void discardRestart(String nonce) throws IOException {
        synchronized (GpuDriverStore.class) {
            mkdir(root);
            try (RandomAccessFile lock = new RandomAccessFile(new File(root, "state.lock"), "rw");
                 FileLock held = lock.getChannel().lock()) {
                String[] request = restartRequest();
                if (request == null) {
                    // A previous delete may have succeeded before directory fsync failed.
                    syncDirectory(root);
                    return;
                }
                if (!matches(request, nonce)) throw new IOException("Stale GPU driver restart token");
                if (!new File(root, "restart").delete()) throw new IOException("Cannot discard GPU driver restart");
                syncDirectory(root);
            }
        }
    }

    public String sealedRestartNonce() {
        synchronized (GpuDriverStore.class) {
            try {
                String[] request = restartRequest();
                return request != null && request[0].equals("SEALED") && !request[1].equals(selection()[1]) ? request[1] : null;
            } catch (IOException e) { return null; }
        }
    }

    /** Only a fresh coordinator may consume the helper-delivered sealed token, before prepare(). */
    public boolean consumeRestart(String nonce) throws IOException {
        synchronized (GpuDriverStore.class) {
            mutable();
            if (diagnosticContext()) return false;
            mkdir(root);
            try (RandomAccessFile lock = new RandomAccessFile(new File(root, "state.lock"), "rw");
                 FileLock held = lock.getChannel().lock()) {
                if (nonce == null || nonce.equals(selection()[1])) return false;
                String[] request = restartRequest();
                if (!matches(request, nonce) || !request[0].equals("SEALED") || request[2].equals(SESSION)) return false;
                validateSelection(request[3]);
                boolean retry = request[4].equals("true");
                if (!request[3].equals("system") && markers(request[3]).length != 0 && !retry) return false;
                // The consumed nonce and selection commit together; a crash never replays retry consent.
                atomicWrite(new File(root, "selected"), request[3] + "\n" + nonce);
                if (retry) clearQuarantine(request[3]);
                if (!new File(root, "restart").delete()) throw new IOException("Cannot retire GPU driver restart");
                syncDirectory(root);
                return true;
            }
        }
    }

    public String[] prepare() {
        synchronized (GpuDriverStore.class) {
            if (frozen) return null;
            frozen = true; // Selection and packages stay fixed until a fresh process, including system launches.
            if (diagnosticContext()) return null;
            String id = selectedId();
            if (id.equals("system") || markers(id).length != 0) return null;
            try {
                File dir = new File(root, id);
                try (RandomAccessFile lock = new RandomAccessFile(new File(root, "state.lock"), "rw");
                     FileLock held = lock.getChannel().lock()) {
                    Driver driver = validateSelection(id);
                    if (markers(id).length != 0) return null;
                    File marker = new File(root, "pending-" + id + "-" + UUID.randomUUID());
                    atomicWrite(marker, id);
                    return new String[] { context.getApplicationInfo().nativeLibraryDir,
                            dir.getAbsolutePath() + File.separator, driver.library, marker.getAbsolutePath(),
                            id.equals(BUNDLED_ID) ? BUNDLED_ID : "" };
                }
            } catch (IOException e) { return null; }
        }
    }

    public Driver importZip(InputStream source) throws IOException {
        File stage;
        synchronized (GpuDriverStore.class) {
            mkdir(root);
            stage = new File(root, ".stage-" + UUID.randomUUID());
            mkdir(stage);
        }
        File archive = new File(stage, ".source.zip");
        try {
            try (FileOutputStream out = new FileOutputStream(archive)) {
                copy(source, out, SOURCE_LIMIT, null); out.getFD().sync();
            }
            checkCentralDirectory(archive);
            Set<String> names = new HashSet<>();
            long total = 0;
            try (ZipFile zip = new ZipFile(archive)) {
                Enumeration<? extends ZipEntry> entries = zip.entries();
                int count = 0;
                while (entries.hasMoreElements()) {
                    ZipEntry entry = entries.nextElement();
                    if (++count > 128) throw new IOException("Too many ZIP entries");
                    String name = entry.getName();
                    if (!name.matches("[A-Za-z0-9_.-]+") || name.equals(".") || name.equals("..") ||
                            name.startsWith(".") || entry.isDirectory() || !names.add(name))
                        throw new IOException("Only unique root-level package files are supported");
                    File target = new File(stage, name);
                    CRC32 crc = new CRC32();
                    long size;
                    try (InputStream in = zip.getInputStream(entry);
                         FileOutputStream out = new FileOutputStream(target)) {
                        size = copy(in, out, Math.min(TOTAL_LIMIT - total,
                                name.equals("meta.json") ? 16384 : FILE_LIMIT), crc);
                        out.getFD().sync();
                    }
                    total += size;
                    if (total > TOTAL_LIMIT || size != entry.getSize() || crc.getValue() != entry.getCrc())
                        throw new IOException("Corrupt or oversized ZIP");
                    if (name.endsWith(".so")) checkElf(target);
                }
            }
            if (!archive.delete()) throw new IOException("Cannot remove source archive");
            Driver metadata = readDriver(stage);
            protectPackage(stage);
            synchronized (GpuDriverStore.class) {
                File destination = new File(root, "pkg-" + UUID.randomUUID());
                if (destination.exists() || !stage.renameTo(destination)) throw new IOException("Cannot publish driver");
                syncDirectory(root);
                return new Driver(destination.getName(), metadata.name, metadata.version, metadata.library);
            }
        } finally {
            removeStage(stage);
        }
    }

    private static void mutable() throws IOException {
        if (frozen) throw new IOException("Restart the app before changing GPU drivers");
    }
    private boolean diagnosticContext() {
        if (!(context instanceof Activity)) return false;
        android.content.Intent intent = ((Activity) context).getIntent();
        return intent != null && (intent.hasExtra("diag_run_id") || intent.hasExtra("run_id"));
    }
    private String[] selection() throws IOException {
        File file = new File(root, "selected");
        if (!file.exists()) return new String[] { "system", "" };
        String[] fields = new String(readSmall(file), StandardCharsets.UTF_8).split("\n", -1);
        if (fields.length < 1 || fields.length > 2 || !validId(fields[0]) ||
                (fields.length == 2 && !validNonce(fields[1]))) throw new IOException("Invalid driver selection state");
        return new String[] { fields[0], fields.length == 2 ? fields[1] : "" };
    }
    private String[] restartRequest() throws IOException {
        File file = new File(root, "restart");
        if (!file.exists()) return null;
        String[] fields = new String(readSmall(file), StandardCharsets.UTF_8).split("\n", -1);
        if (fields.length != 5 || !(fields[0].equals("STAGED") || fields[0].equals("SEALED")) ||
                !validNonce(fields[1]) || !validNonce(fields[2]) || !validId(fields[3]) ||
                !(fields[4].equals("true") || fields[4].equals("false")))
            throw new IOException("Invalid GPU driver restart state");
        return fields;
    }
    private static boolean validNonce(String nonce) {
        return nonce != null && nonce.matches("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
    }
    private static boolean matches(String[] request, String nonce) {
        return request != null && validNonce(nonce) && request[1].equals(nonce);
    }
    private Driver validateSelection(String id) throws IOException {
        if (!validId(id)) throw new IOException("Invalid driver selection");
        if (id.equals("system")) return null;
        File dir = new File(root, id);
        return id.equals(BUNDLED_ID) ? validateBundled(dir) : readDriver(dir);
    }
    private void clearQuarantine(String id) throws IOException {
        if (id.equals("system")) return;
        for (File marker : markers(id))
            if (!marker.delete()) throw new IOException("Cannot clear driver quarantine");
        syncDirectory(root);
    }
    private static Driver validateBundled(File dir) throws IOException {
        Driver driver = readDriver(dir);
        if (!driver.library.equals(BUNDLED_LIBRARY) || !driver.name.equals("Turnip — Adreno 830") ||
                !driver.version.equals("Mesa 26.3.0-devel (Unleashed)") || Build.VERSION.SDK_INT < 35)
            throw new IOException("Invalid bundled driver metadata");
        try {
            JSONObject meta = new JSONObject(new String(readSmall(new File(dir, "meta.json")), StandardCharsets.UTF_8));
            JSONObject manifest = new JSONObject(new String(readSmall(new File(dir, "bundle-manifest.json")), StandardCharsets.UTF_8));
            if (!(meta.get("minApi") instanceof Number) || ((Number) meta.get("minApi")).doubleValue() != 35 ||
                    !BUNDLED_ID.equals(text(manifest, "profileId")) || !BUNDLED_HASH.equals(text(manifest, "sha256")) ||
                    !(manifest.get("sizeBytes") instanceof Number) ||
                    ((Number) manifest.get("sizeBytes")).doubleValue() != BUNDLED_SIZE)
                throw new IOException("Invalid bundled driver manifest");
        } catch (IOException e) { throw e; }
        catch (Exception e) { throw new IOException("Malformed bundled driver manifest", e); }
        File library = new File(dir, BUNDLED_LIBRARY);
        if (library.length() != BUNDLED_SIZE) throw new IOException("Bundled driver size mismatch");
        MessageDigest digest;
        try { digest = MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException e) { throw new IOException("SHA-256 unavailable", e); }
        try (InputStream in = new DigestInputStream(new FileInputStream(library), digest)) {
            byte[] buffer = new byte[8192];
            long total = 0; int count;
            while ((count = in.read(buffer)) != -1) {
                total += count;
                if (total > BUNDLED_SIZE) throw new IOException("Bundled driver size limit exceeded");
            }
            if (total != BUNDLED_SIZE) throw new IOException("Bundled driver size mismatch");
        }
        StringBuilder hash = new StringBuilder();
        for (byte value : digest.digest()) hash.append(String.format(Locale.ROOT, "%02x", value & 255));
        if (!BUNDLED_HASH.equals(hash.toString())) throw new IOException("Bundled driver SHA-256 mismatch");
        return new Driver(BUNDLED_ID, driver.name, driver.version, driver.library);
    }
    private static void protectPackage(File stage) throws IOException {
        File[] contents = stage.listFiles();
        if (contents == null) throw new IOException("Cannot list staged package");
        for (File file : contents)
            if (!file.setReadOnly()) throw new IOException("Cannot protect driver content");
        syncDirectory(stage);
    }
    private static void removeStage(File stage) {
        // Never remove any published package or another import's staging directory.
        File[] files = stage.listFiles();
        if (files != null) for (File file : files) { file.setWritable(true); file.delete(); }
        stage.delete();
    }
    private static boolean validId(String id) {
        return id != null && (id.equals("system") || id.equals(BUNDLED_ID) || id.matches("pkg-[0-9a-f-]{36}"));
    }
    private File[] markers(String id) {
        File[] files = root.listFiles((dir, name) -> name.startsWith("pending-" + id + "-"));
        return files == null ? new File[0] : files;
    }
    private static Driver readDriver(File dir) throws IOException {
        try {
            JSONObject meta = new JSONObject(new String(readSmall(new File(dir, "meta.json")), StandardCharsets.UTF_8));
            String name = text(meta, "name"), version = text(meta, meta.has("version") ? "version" : "packageVersion"), library = text(meta, "libraryName");
            if (!library.matches("[A-Za-z0-9_-][A-Za-z0-9_.-]*\\.so")) throw new IOException("Invalid libraryName");
            if (meta.has("minApi")) {
                Object api = meta.get("minApi");
                if (!(api instanceof Number) || ((Number) api).doubleValue() != ((Number) api).intValue() ||
                        ((Number) api).intValue() < 1 || ((Number) api).intValue() > Build.VERSION.SDK_INT)
                    throw new IOException("Incompatible minApi");
            }
            checkElf(new File(dir, library));
            return new Driver(dir.getName(), name, version, library);
        } catch (IOException e) { throw e; }
        catch (Exception e) { throw new IOException("Malformed driver metadata", e); }
    }
    private static String text(JSONObject object, String key) throws Exception {
        Object value = object.get(key);
        if (!(value instanceof String) || ((String) value).trim().isEmpty() || ((String) value).length() > 256)
            throw new IOException("Invalid metadata " + key);
        return (String) value;
    }
    private static byte[] readSmall(File file) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (InputStream in = new FileInputStream(file)) { copy(in, out, 16384, null); }
        return out.toByteArray();
    }
    private static long copy(InputStream in, OutputStream out, long limit, CRC32 crc) throws IOException {
        byte[] buffer = new byte[8192]; long total = 0; int n;
        while ((n = in.read(buffer)) != -1) {
            total += n;
            if (total > limit) throw new IOException("Driver size limit exceeded");
            out.write(buffer, 0, n);
            if (crc != null) crc.update(buffer, 0, n);
        }
        return total;
    }
    private static void checkElf(File file) throws IOException {
        byte[] h = new byte[64];
        try (DataInputStream in = new DataInputStream(new FileInputStream(file))) { in.readFully(h); }
        if (h[0] != 127 || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || h[4] != 2 || h[5] != 1 ||
                h[6] != 1 || h[16] != 3 || h[17] != 0 || (h[18] & 255) != 183 || h[19] != 0)
            throw new IOException("Driver must be little-endian AArch64 ELF64 ET_DYN");
    }
    private static void mkdir(File dir) throws IOException {
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Cannot create driver directory");
    }
    private static void atomicWrite(File file, String text) throws IOException {
        File temp = new File(file.getParentFile(), ".write-" + UUID.randomUUID());
        try {
            try (FileOutputStream out = new FileOutputStream(temp)) {
                out.write(text.getBytes(StandardCharsets.UTF_8)); out.getFD().sync();
            }
            try { Os.rename(temp.getAbsolutePath(), file.getAbsolutePath()); }
            catch (Exception e) { throw new IOException("Cannot persist driver state", e); }
            syncDirectory(file.getParentFile());
        } finally { temp.delete(); }
    }
    private static void syncDirectory(File dir) throws IOException {
        try {
            java.io.FileDescriptor fd = Os.open(dir.getAbsolutePath(), OsConstants.O_RDONLY, 0);
            try { Os.fsync(fd); } finally { Os.close(fd); }
        } catch (Exception e) { throw new IOException("Cannot sync driver directory", e); }
    }

    // ZipFile alone accepts truncated archives and does not expose Unix symlink attributes.
    private static void checkCentralDirectory(File file) throws IOException {
        try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
            long end = -1;
            for (long p = in.length() - 22, min = Math.max(0, in.length() - 65557); p >= min; p--) {
                in.seek(p);
                if (u32(in) == 0x06054b50L) {
                    in.seek(p + 20);
                    if (p + 22 + u16(in) == in.length()) { end = p; break; }
                }
            }
            if (end < 0) throw new IOException("Missing ZIP end record");
            in.seek(end + 4);
            if (u16(in) != 0 || u16(in) != 0) throw new IOException("Split ZIP unsupported");
            int count = u16(in), total = u16(in);
            long size = u32(in), offset = u32(in);
            if (count != total || total == 0 || total > 128 || offset + size != end)
                throw new IOException("Invalid ZIP directory");
            in.seek(offset);
            for (int i = 0; i < total; i++) {
                long start = in.getFilePointer();
                if (u32(in) != 0x02014b50L) throw new IOException("Invalid ZIP entry");
                in.seek(start + 8);
                if ((u16(in) & 1) != 0) throw new IOException("Encrypted ZIP unsupported");
                in.seek(start + 28);
                int name = u16(in), extra = u16(in), comment = u16(in);
                if (u16(in) != 0) throw new IOException("Split ZIP entry");
                in.seek(start + 38);
                long attributes = u32(in);
                int type = (int) (attributes >>> 16) & 0170000;
                if (type != 0 && type != 0100000) throw new IOException("Non-regular ZIP entry");
                in.seek(start + 46L + name + extra + comment);
                if (in.getFilePointer() > end) throw new IOException("Truncated ZIP directory");
            }
            if (in.getFilePointer() != end) throw new IOException("Ambiguous ZIP directory");
        }
    }
    private static int u16(RandomAccessFile in) throws IOException {
        return in.readUnsignedByte() | (in.readUnsignedByte() << 8);
    }
    private static long u32(RandomAccessFile in) throws IOException {
        return (long) u16(in) | ((long) u16(in) << 16);
    }
}
