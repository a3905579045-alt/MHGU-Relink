import java.io.*;
import java.nio.file.*;
import java.util.*;

// The runner appends prepareForgeAssets and copyAsset unchanged from GameActivity.
class ForgeBuiltinTest {
    private static final String TAG = "test";
    private final File internal;
    private final AssetFiles assets;
    private static class Log {
        static int warnings;
        static void w(String tag, String message) { ++warnings; }
    }
    private static class AssetFiles {
        final Path root;
        AssetFiles(Path root) { this.root = root; }
        InputStream open(String name) throws IOException { return new FileInputStream(root.resolve(name).toFile()); }
    }
    ForgeBuiltinTest(Path internal, Path assets) {
        this.internal = internal.toFile();
        this.assets = new AssetFiles(assets);
    }
    private File getFilesDir() { return internal; }
    private AssetFiles getAssets() { return assets; }
    private static void checkAssets(Path internal, Path assets) throws Exception {
        try (var files = Files.walk(assets.resolve("forge-v1.5.1"))) {
            List<Path> resources = files.filter(Files::isRegularFile).toList();
            assert resources.size() == 5;
            for (Path source : resources)
                assert Arrays.equals(Files.readAllBytes(source), Files.readAllBytes(internal.resolve(assets.relativize(source))));
        }
    }
    public static void main(String[] args) throws Exception {
        Path internal = Path.of(args[0]), assets = Path.of(args[1]);
        Path userConfig = internal.resolve("mods/User/romfs/nativeNX/forge/config.ini");
        Files.createDirectories(userConfig.getParent());
        Files.writeString(userConfig, "user settings\n");
        ForgeBuiltinTest test = new ForgeBuiltinTest(internal, assets);
        test.prepareForgeAssets();
        checkAssets(internal, assets);
        test.prepareForgeAssets();
        checkAssets(internal, assets);
        Path config = internal.resolve("forge-v1.5.1/romfs/nativeNX/forge/config.ini");
        Files.writeString(config, "partial");
        test.prepareForgeAssets();
        checkAssets(internal, assets);
        assert Files.readString(userConfig).equals("user settings\n");
        assert Log.warnings == 0;
        new ForgeBuiltinTest(internal.resolve("missing-assets"), assets.resolve("absent")).prepareForgeAssets();
        assert Log.warnings == 5;
        System.out.println("PASS: production Java copies all five resources, supports repeated/repaired preparation, preserves user config and tolerates missing assets");
    }
    // PRODUCTION_METHODS
}
