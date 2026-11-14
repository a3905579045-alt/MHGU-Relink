// Executes the production private store method. Android Bitmap decoding is not invoked.
import java.io.File;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.FileTime;
import java.security.MessageDigest;
import java.security.Permission;
import java.util.Arrays;

public final class TouchIconStoreHostTest {
    private static Method store;
    private static int groups;
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    private static String invoke(Path root, byte[] encoded) throws Exception {
        try { return (String) store.invoke(null, root.toFile(), encoded); }
        catch (InvocationTargetException exception) {
            Throwable cause = exception.getCause();
            if (cause instanceof Exception) throw (Exception) cause;
            throw new AssertionError(cause);
        }
    }
    private static byte[] icon(int color) {
        byte[] bytes = new byte[8 + 2 * 2 * 4];
        bytes[0] = 'T'; bytes[1] = 'I'; bytes[2] = 'C'; bytes[3] = 'O';
        bytes[4] = bytes[6] = 2;
        for (int i = 8; i < bytes.length; ++i) bytes[i] = (byte) (color + i);
        return bytes;
    }
    private static String hash(byte[] bytes) throws Exception {
        StringBuilder out = new StringBuilder();
        for (byte value : MessageDigest.getInstance("SHA-256").digest(bytes))
            out.append(String.format("%02x", value & 255));
        return out.toString();
    }
    private static Path target(Path root, String hash) { return root.resolve("touch_icons").resolve(hash + ".rgba"); }
    private static void noParts(Path root) throws Exception {
        Path dir = root.resolve("touch_icons");
        try (java.util.stream.Stream<Path> paths = Files.list(dir)) {
            check(paths.noneMatch(p -> p.getFileName().toString().endsWith(".part")), "temporary import file leaked");
        }
    }
    private static void dedup(Path root) throws Exception {
        byte[] encoded = icon(1);
        String actual = invoke(root, encoded);
        check(actual.matches("[a-f0-9]{64}") && actual.equals(hash(encoded)), "content hash mismatch");
        Path target = target(root, actual);
        check(Arrays.equals(Files.readAllBytes(target), encoded), "stored bytes differ");
        Files.setLastModifiedTime(target, FileTime.fromMillis(1234567890000L));
        FileTime modified = Files.getLastModifiedTime(target);
        check(invoke(root, encoded.clone()).equals(actual), "duplicate hash differs");
        check(Files.getLastModifiedTime(target).equals(modified), "duplicate rewrote immutable target");
        noParts(root); ++groups;
        System.out.println("PASS: real store creates SHA-256 named content and deduplicates identical bytes without rewriting.");
    }
    private static void corruptExisting(Path root, boolean resize) throws Exception {
        byte[] encoded = icon(resize ? 7 : 9);
        String actual = invoke(root, encoded);
        Path target = target(root, actual);
        byte[] broken = resize ? Arrays.copyOf(encoded, encoded.length - 1) : encoded.clone();
        if (!resize) broken[broken.length - 1] ^= 0x55;
        Files.write(target, broken);
        boolean failed = false;
        try { invoke(root, encoded); } catch (IOException expected) { failed = true; }
        check(failed, "corrupt existing content was accepted");
        check(Arrays.equals(Files.readAllBytes(target), broken), "corrupt target was silently overwritten");
        noParts(root); ++groups;
        System.out.println("PASS: existing " + (resize ? "size" : "hash/content") + " mismatch fails without overwriting the target.");
    }
    private static void sameName(Path root) throws Exception {
        Path a = root.resolve("origin-a/same-name.png"), b = root.resolve("origin-b/same-name.png");
        Files.createDirectories(a.getParent()); Files.createDirectories(b.getParent());
        // Predecoded TICO fixtures; filenames are deliberately identical and are not passed to store.
        Files.write(a, icon(12)); Files.write(b, icon(29));
        String one = invoke(root, Files.readAllBytes(a)), two = invoke(root, Files.readAllBytes(b));
        check(!one.equals(two), "different contents collided because of an original filename");
        check(Arrays.equals(Files.readAllBytes(target(root, one)), icon(12)), "first content lost");
        check(Arrays.equals(Files.readAllBytes(target(root, two)), icon(29)), "second content lost");
        noParts(root); ++groups;
        System.out.println("PASS: different contents with the same original filename occupy independent immutable hash files.");
    }
    @SuppressWarnings("removal")
    private static void moveFailure(Path root) throws Exception {
        byte[] encoded = icon(43);
        String digest = hash(encoded);
        Path destination = target(root, digest).toAbsolutePath();
        SecurityManager previous = System.getSecurityManager();
        boolean failed = false;
        try {
            System.setSecurityManager(new SecurityManager() {
                @Override public void checkPermission(Permission permission) {}
                @Override public void checkWrite(String path) {
                    if (new File(path).toPath().toAbsolutePath().equals(destination))
                        throw new SecurityException("fixture denies final move only");
                }
            });
            try { invoke(root, encoded); } catch (SecurityException expected) { failed = true; }
        } finally { System.setSecurityManager(previous); }
        check(failed && !Files.exists(destination), "move failure fixture did not trigger");
        noParts(root); ++groups;
        System.out.println("PASS: failure after writing the part file leaves no target and the production finally block removes the part.");
    }
    private static void badDirectory(Path root) throws Exception {
        Files.createDirectories(root);
        byte[] sentinel = {1,2,3}; Files.write(root.resolve("touch_icons"), sentinel);
        boolean failed = false;
        try { invoke(root, icon(50)); } catch (IOException expected) { failed = true; }
        check(failed && Arrays.equals(Files.readAllBytes(root.resolve("touch_icons")), sentinel), "folder failure overwrote existing data");
        ++groups;
        System.out.println("PASS: unavailable icon directory fails without overwriting the existing file.");
    }
    public static void main(String[] args) throws Exception {
        check(args.length == 1, "supply the evidence output directory");
        Path output = new File(args[0]).toPath(); Files.createDirectories(output);
        Path root = Files.createTempDirectory(output, "storage-fixture-");
        Class<?> type = Class.forName("com.mhgu.relink.TouchIconImport");
        store = type.getDeclaredMethod("store", File.class, byte[].class); store.setAccessible(true);
        dedup(root.resolve("dedup")); corruptExisting(root.resolve("wrong-size"), true);
        corruptExisting(root.resolve("wrong-content"), false); sameName(root.resolve("same-name"));
        moveFailure(root.resolve("move-failure")); badDirectory(root.resolve("bad-directory"));
        check(groups == 6, "missing storage cases");
        System.out.println("PASS_ICON_STORAGE_GROUPS=6");
        System.out.println("Android Bitmap decoding, picker interaction and device rendering were not executed.");
    }
}
