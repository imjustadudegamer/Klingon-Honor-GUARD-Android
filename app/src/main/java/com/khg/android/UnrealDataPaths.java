package com.khg.android;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.os.Build;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class UnrealDataPaths {
    static final String TAG_STARTUP = "UE1Startup";
    static final String TAG_CONFIG = "UE1Config";
    static final String TAG_IMPORT = "UE1Import";
    static final String EXTRA_UNREAL_ROOT = "com.khg.android.EXTRA_UNREAL_ROOT";

    static final String[] UNREAL_DIRS = {
            "System", "Maps", "Textures", "Sounds", "Music", "Meshes", "Help", "Web", "Save", "Cache"
    };

    private UnrealDataPaths() {}


    static final class ImportResult {
        final boolean ok;
        final File root;
        final String message;

        private ImportResult(boolean ok, File root, String message) {
            this.ok = ok;
            this.root = root;
            this.message = message;
        }

        static ImportResult ok(File root, String message) {
            return new ImportResult(true, root, message);
        }

        static ImportResult fail(String message) {
            return new ImportResult(false, null, message);
        }

        static ImportResult fail(String message, Throwable t) {
            Log.e(TAG_IMPORT, message, t);
            return new ImportResult(false, null, message + "\n\n" + t.getClass().getSimpleName() + ": " + t.getMessage());
        }
    }

    private static final class SafNode {
        final String docId;
        final String name;
        final String mimeType;

        SafNode(String docId, String name, String mimeType) {
            this.docId = docId;
            this.name = name;
            this.mimeType = mimeType;
        }

        boolean isDirectory() {
            return Document.MIME_TYPE_DIR.equals(mimeType);
        }
    }

    private static final class ZipRootFlags {
        boolean core;
        boolean engine;
        boolean klingons;
        boolean map;

        boolean valid() {
            return core && engine && klingons && map;
        }

        int score() {
            int s = 0;
            if (core) s++;
            if (engine) s++;
            if (klingons) s += 2;
            if (map) s += 2;
            return s;
        }
    }

    static File primaryAppRoot(Context context) {
        File base = context.getExternalFilesDir(null);
        if (base == null) base = context.getFilesDir();
        return new File(base, "Unreal");
    }

    static File findBestUnrealRoot(Context context) {
        List<File> candidates = candidateRoots(context);
        for (File candidate : candidates) {
            boolean valid = hasRequiredData(candidate);
            Log.i(TAG_STARTUP, "data candidate: valid=" + valid + " root=" + candidate.getAbsolutePath());
            if (valid) return candidate;
        }
        File fallback = primaryAppRoot(context);
        Log.i(TAG_STARTUP, "data fallback root=" + fallback.getAbsolutePath());
        return fallback;
    }

    static List<File> candidateRoots(Context context) {
        ArrayList<File> out = new ArrayList<>();
        HashSet<String> seen = new HashSet<>();

        addCandidate(out, seen, primaryAppRoot(context));
        File[] appExternalDirs = context.getExternalFilesDirs(null);
        if (appExternalDirs != null) {
            for (File appDir : appExternalDirs) {
                if (appDir == null) continue;
                addCandidate(out, seen, new File(appDir, "Unreal"));
            }
        }

        try {
            File publicRoot = Environment.getExternalStorageDirectory();
            if (publicRoot != null) addCandidate(out, seen, new File(publicRoot, "Unreal"));
        } catch (Throwable ignored) {}
        addCandidate(out, seen, new File("/storage/emulated/0/Unreal"));
        addCandidate(out, seen, new File("/sdcard/Unreal"));
        addCandidate(out, seen, new File("/storage/sdcard0/Unreal"));
        addCandidate(out, seen, new File("/mnt/sdcard/Unreal"));
        addCandidate(out, seen, new File("/mnt/usbdrive/Unreal"));
        addCandidate(out, seen, new File("/mnt/usbdrive0/Unreal"));
        addCandidate(out, seen, new File("/mnt/usb_storage/Unreal"));

        if (appExternalDirs != null) {
            for (File appDir : appExternalDirs) {
                File storageRoot = storageRootFromExternalFilesDir(appDir);
                if (storageRoot != null) addCandidate(out, seen, new File(storageRoot, "Unreal"));
            }
        }

        File[] volumes = new File("/storage").listFiles();
        if (volumes != null) {
            for (File volume : volumes) {
                String name = volume.getName();
                if ("self".equals(name) || "emulated".equals(name)) continue;
                addCandidate(out, seen, new File(volume, "Unreal"));
            }
        }

        File[] mntVolumes = new File("/mnt").listFiles();
        if (mntVolumes != null) {
            for (File volume : mntVolumes) {
                String name = volume.getName();
                if ("runtime".equals(name) || "asec".equals(name) || "obb".equals(name)) continue;
                addCandidate(out, seen, new File(volume, "Unreal"));
            }
        }
        return out;
    }

    private static File storageRootFromExternalFilesDir(File appDir) {
        if (appDir == null) return null;
        File p = appDir;
        for (int i = 0; i < 4 && p != null; ++i) p = p.getParentFile();
        return p;
    }

    private static void addCandidate(ArrayList<File> out, HashSet<String> seen, File candidate) {
        if (candidate == null) return;
        try { candidate = candidate.getCanonicalFile(); } catch (IOException ignored) { candidate = candidate.getAbsoluteFile(); }
        String key = candidate.getAbsolutePath();
        if (seen.add(key)) out.add(candidate);
    }

    static boolean hasRequiredData(File root) {
        return hasRequiredData(root, false);
    }

    static boolean hasRequiredData(File root, boolean verbose) {
        if (root == null) return false;
        File systemDir = new File(root, "System");
        File mapsDir = new File(root, "Maps");
        boolean rootDir = root.isDirectory();
        boolean system = systemDir.isDirectory();
        boolean maps = mapsDir.isDirectory();
        boolean hasCore = findCaseInsensitive(systemDir, "Core.u") != null;
        boolean hasEngine = findCaseInsensitive(systemDir, "Engine.u") != null;
        boolean hasGamePackage = findCaseInsensitive(systemDir, "Klingons.u") != null;
        boolean hasMap = findCaseInsensitive(mapsDir, "Entry.unr") != null || hasAnyMap(mapsDir);
        boolean ok = rootDir && system && maps && hasCore && hasEngine && hasGamePackage && hasMap;
        if (verbose || root.exists()) {
            Log.i(TAG_STARTUP, "data candidate detail: ok=" + ok
                    + " rootDir=" + rootDir
                    + " system=" + system
                    + " maps=" + maps
                    + " core=" + hasCore
                    + " engine=" + hasEngine
                    + " game=" + hasGamePackage
                    + " map=" + hasMap
                    + " root=" + root.getAbsolutePath());
        }
        return ok;
    }

    private static File findCaseInsensitive(File dir, String expectedName) {
        if (dir == null || expectedName == null) return null;
        File exact = new File(dir, expectedName);
        if (exact.isFile()) return exact;
        File[] files = dir.listFiles();
        if (files == null) return null;
        for (File f : files) {
            if (f.isFile() && expectedName.equalsIgnoreCase(f.getName())) return f;
        }
        return null;
    }

    private static boolean hasAnyMap(File mapsDir) {
        File[] files = mapsDir.listFiles((dir, name) -> name.toLowerCase(Locale.ROOT).endsWith(".unr"));
        return files != null && files.length > 0;
    }

    static void ensureDirectoryLayout(File root) {
        if (root == null) return;
        for (String dir : UNREAL_DIRS) {
            File target = new File(root, dir);
            if (!target.exists() && !target.mkdirs()) Log.w(TAG_CONFIG, "Could not create directory: " + target.getAbsolutePath());
        }
    }

    static void installDefaultConfigsIfNeeded(Context context, File root) {
        if (root == null) return;
        File systemDir = new File(root, "System");
        if (!systemDir.exists() && !systemDir.mkdirs()) Log.w(TAG_CONFIG, "Could not create System directory: " + systemDir.getAbsolutePath());
        // UNREAL_ANDROID_SINGLE_INI_V148: Unreal.ini is the ONE engine config file (it is the DefaultIni the
        // engine loads AND saves — UnConfig.cpp/UnPlat.cpp). Default.ini used to be shipped and patched in
        // parallel, but it is only a first-run copy SEED (UnPlat.cpp copies it to Unreal.ini only when
        // Unreal.ini is missing) and is never read at runtime, so it was pure dual-maintenance. We no longer
        // ship or touch it; everything is unified into Unreal.ini.
        copyAssetIfMissing(context, "ue1_config/Unreal.ini", new File(systemDir, "Unreal.ini"));
        copyAssetIfMissing(context, "ue1_config/User.ini", new File(systemDir, "User.ini"));
        copyAssetIfMissing(context, "ue1_config/AndroidController.ini", new File(systemDir, "AndroidController.ini"));
        copyAssetIfMissing(context, "ue1_config/AndroidUI.ini", new File(systemDir, "AndroidUI.ini"));
    }

    private static void copyAssetIfMissing(Context context, String asset, File out) {
        if (out.isFile()) return;
        try (InputStream input = context.getAssets().open(asset); FileOutputStream fos = new FileOutputStream(out)) {
            byte[] buf = new byte[16 * 1024];
            int read;
            while ((read = input.read(buf)) >= 0) fos.write(buf, 0, read);
            fos.flush();
            Log.i(TAG_CONFIG, "Installed default config: " + out.getAbsolutePath());
        } catch (IOException ex) {
            Log.w(TAG_CONFIG, "Could not install default config " + out.getAbsolutePath() + ": " + ex);
        }
    }

    static void normalizeConfigForDetectedData(File root) {
        if (root == null) return;
        File systemDir = new File(root, "System");
        if (findCaseInsensitive(systemDir, "Klingons.u") == null) return;
        patchPackageName(new File(root, "System/Unreal.ini"));
    }

    private static void patchPackageName(File file) {
        if (!file.isFile()) return;
        try {
            String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            String patched = text
                    .replace("DefaultGame=UnrealI.SinglePlayer", "DefaultGame=Klingons.SinglePlayer")
                    .replace("DefaultGame=UnrealShare.SinglePlayer", "DefaultGame=Klingons.SinglePlayer")
                    .replace("DefaultServerGame=UnrealI.DeathMatchGame", "DefaultServerGame=Klingons.DeathMatchGame")
                    .replace("DefaultServerGame=UnrealShare.DeathMatchGame", "DefaultServerGame=Klingons.DeathMatchGame")
                    .replace("DefaultGame=UnrealI.", "DefaultGame=Klingons.")
                    .replace("DefaultGame=UnrealShare.", "DefaultGame=Klingons.")
                    .replace("DefaultServerGame=UnrealI.", "DefaultServerGame=Klingons.")
                    .replace("DefaultServerGame=UnrealShare.", "DefaultServerGame=Klingons.");
            if (!patched.equals(text)) {
                Files.write(file.toPath(), patched.getBytes(StandardCharsets.UTF_8));
                Log.i(TAG_CONFIG, "Patched retail v200 config package names: " + file.getAbsolutePath());
            }
        } catch (IOException ex) {
            Log.w(TAG_CONFIG, "Could not patch config " + file.getAbsolutePath() + ": " + ex);
        }
    }

    static void ensureWritableConfigFiles(Context context, File root) {
        if (root == null) return;
        try {
            ensureDirectoryLayout(root);
            installDefaultConfigsIfNeeded(context, root);
            File systemDir = new File(root, "System");
            // UNREAL_ANDROID_SINGLE_INI_V148: User.ini is an inert stub — all config lives in Unreal.ini.
            // If it is somehow missing, seed an empty, comment-only file (no [Engine.Input]/[DefaultPlayer],
            // which would be dead weight and a drift trap).
            ensureConfigFile(systemDir, "User.ini", new String[] { "DefUser.ini", "DefaultUser.ini" },
                    "; Inert stub — all config lives in Unreal.ini (see the shipped User.ini header).\n");
            ensureConfigFile(systemDir, "Unreal.ini", new String[] { "Unreal.ini.default" }, "");
            ensureAndroidControllerDirectPatch(systemDir);
            Log.i(TAG_CONFIG, "Config root: " + root.getAbsolutePath());
            Log.i(TAG_CONFIG, "User.ini: " + new File(systemDir, "User.ini").getAbsolutePath());
        } catch (Throwable t) {
            Log.e(TAG_CONFIG, "Config bootstrap failed for root=" + root.getAbsolutePath(), t);
        }
    }


    private static void ensureAndroidControllerDirectPatch(File systemDir) {
        // UNREAL_ANDROID_CONFIG_PRESERVE_V139
        // Keep the Android controller/touch runtime flags available, but do not
        // re-append or rewrite [Engine.Input] on every app update.  Reinstalling
        // the APK retains /Android/data, therefore existing user key bindings must
        // be treated as authoritative.
        // UNREAL_ANDROID_SINGLE_INI_V148: all engine config lives in Unreal.ini (the DefaultIni). Default.ini
        // and User.ini are NOT read for game config at runtime, so we patch ONLY Unreal.ini. The old
        // User.ini writers (appendControllerInputFallbacks / ensureMenuKeyBinding) are gone — they wrote
        // bindings that never loaded and only re-polluted an inert file. Escape=ShowMenu and the controller
        // fallbacks are covered by ensureInputBindings on Unreal.ini below.
        patchNsdlControllerDefaults(new File(systemDir, "Unreal.ini"));
        repairRenderModule(new File(systemDir, "Unreal.ini"));
        repairCoreConfig(new File(systemDir, "Unreal.ini"));
        ensureInputBindings(new File(systemDir, "Unreal.ini"));
    }

    // UNREAL_ANDROID_INPUT_BINDINGS_IN_DEFAULTINI_V144: UE1 loads AND saves [Engine.Input] from the
    // DefaultIni (Unreal.ini), not User.ini. This port shipped its bindings only in User.ini, so they never
    // loaded and fire/alt/move/look (which go through CauseInputEvent on config-bound keys) did nothing,
    // while jump/crouch/next worked because they bind just-in-time. Fresh installs now get the full
    // [Engine.Input] block from the shipped Unreal.ini; this heals existing devices whose Unreal.ini
    // predates it. set-if-absent so in-game rebinds (SaveConfig also writes them to Unreal.ini) persist.
    private static final String[][] DEFAULT_INPUT_BINDINGS = {
        {"Escape","ShowMenu"},{"Enter","InventoryActivate"},
        {"LeftMouse","Fire"},{"RightMouse","AltFire"},
        {"MouseX","Axis aMouseX Speed=6.0"},{"MouseY","Axis aMouseY Speed=6.0"},
        {"W","MoveForward"},{"S","MoveBackward"},{"A","StrafeLeft"},{"D","StrafeRight"},
        {"Space","Jump"},{"C","Duck"},{"G","Grab"},
        {"Joy1","Jump"},{"Joy2","Duck"},{"Joy3","Use"},{"Joy4","NextWeapon"},{"Joy5","ActivateTranslator"},
        {"Joy8","Duck"},{"Joy9","CenterView"},{"Joy10","PrevWeapon"},{"Joy11","NextWeapon"},
        {"Joy12","AltFire"},{"Joy13","Fire"},{"Joy14","TurnLeft"},{"Joy15","TurnRight"},{"Joy16","LookUp"},
        {"JoyX","Axis aStrafe Speed=1"},{"JoyY","Axis aBaseY Speed=1"},
        {"JoyU","Axis aTurn Speed=1"},{"JoyV","Axis aLookUp Speed=-1"},
        {"JoyPovRight","NextWeapon"},{"JoyPovLeft","PrevWeapon"},
        {"JoyPovUp","InventoryPrevious"},{"JoyPovDown","InventoryNext"},
        {"UnknownD8","StrafeLeft"},{"UnknownD9","StrafeRight"},
        {"UnknownDA","MoveForward"},{"UnknownDF","MoveBackward"},{"UnknownEA","LookDown"},
        // UNREAL_ANDROID_INPUT_ALIASES_V148: the alias table that DEFINES what the bound command
        // names mean. In UE1 "Duck", "StrafeLeft", "MoveForward", "TurnLeft" etc. are NOT built-in
        // commands — they are aliases here that expand to "Axis aStrafe ..."/"Button bDuck ...".
        // Without this table those bindings resolve to nothing, so on-screen crouch and the analog/
        // digital move stick (and keyboard WASD) do nothing, while Jump/Fire still work because
        // "Jump"/"Fire" are also PlayerPawn exec functions (the first token of their own alias).
        {"Aliases[0]","(Command=\"Button bFire | Fire\",Alias=Fire)"},
        {"Aliases[1]","(Command=\"Button bAltFire | AltFire\",Alias=AltFire)"},
        {"Aliases[2]","(Command=\"Axis aBaseY  Speed=+300.0\",Alias=MoveForward)"},
        {"Aliases[3]","(Command=\"Axis aBaseY  Speed=-300.0\",Alias=MoveBackward)"},
        {"Aliases[4]","(Command=\"Axis aBaseX Speed=-150.0\",Alias=TurnLeft)"},
        {"Aliases[5]","(Command=\"Axis aBaseX  Speed=+150.0\",Alias=TurnRight)"},
        {"Aliases[6]","(Command=\"Axis aStrafe Speed=-300.0\",Alias=StrafeLeft)"},
        {"Aliases[7]","(Command=\"Axis aStrafe Speed=+300.0\",Alias=StrafeRight)"},
        {"Aliases[8]","(Command=\"Jump | Axis aUp Speed=+300.0\",Alias=Jump)"},
        {"Aliases[9]","(Command=\"Button bDuck | Axis aUp Speed=-300.0\",Alias=Duck)"},
        {"Aliases[10]","(Command=\"Button bLook\",Alias=Look)"},
        {"Aliases[11]","(Command=\"Toggle bLook\",Alias=LookToggle)"},
        {"Aliases[12]","(Command=\"ActivateItem\",Alias=InventoryActivate)"},
        {"Aliases[13]","(Command=\"NextItem\",Alias=InventoryNext)"},
        {"Aliases[14]","(Command=\"PrevItem\",Alias=InventoryPrevious)"},
        {"Aliases[15]","(Command=\"Axis aLookUp Speed=+100.0\",Alias=LookUp)"},
        {"Aliases[16]","(Command=\"Axis aLookUp Speed=-100.0\",Alias=LookDown)"},
        {"Aliases[17]","(Command=\"Button bSnapLevel\",Alias=CenterView)"},
        {"Aliases[18]","(Command=\"Button bRun\",Alias=Walking)"},
        {"Aliases[19]","(Command=\"Button bStrafe\",Alias=Strafe)"},
        {"Aliases[20]","(Command=\"ActivateTranslator\",Alias=ActivateTranslator)"},
        {"Aliases[21]","(Command=\"ActivateHint\",Alias=ActivateHint)"},
    };
    private static void ensureInputBindings(File file) {
        if (file == null || !file.isFile()) return;
        try {
            String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            String p = text;
            for (String[] kv : DEFAULT_INPUT_BINDINGS)
                p = setIniValueIfAbsent(p, "Engine.Input", kv[0], kv[1]);
            if (!p.equals(text)) {
                Files.write(file.toPath(), p.getBytes(StandardCharsets.UTF_8));
                Log.i(TAG_CONFIG, "Ensured [Engine.Input] default bindings: " + file.getAbsolutePath());
            }
        } catch (IOException ex) {
            Log.w(TAG_CONFIG, "Could not ensure input bindings in " + file.getAbsolutePath() + ": " + ex);
        }
    }

    // UNREAL_ANDROID_MENU_KEY_BINDING_V144: bind Escape -> ShowMenu so the in-game menu opens (from the
    // hardware Back key, a gamepad Start, or the on-screen MENU button, all of which deliver IK_Escape).
    // The stock KHG config binds this, but the port's minimal [Engine.Input] set omitted it, so ESC did
    // nothing and "PRESS ESC TO BEGIN" / the menu were unreachable. Set-if-absent so a user rebind wins.
    private static void ensureMenuKeyBinding(File file) {
        if (file == null || !file.isFile()) return;
        try {
            String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            String p = setIniValueIfAbsent(text, "Engine.Input", "Escape", "ShowMenu");
            p = setIniValueIfAbsent(p, "Engine.Input", "Enter", "InventoryActivate");
            if (!p.equals(text)) {
                Files.write(file.toPath(), p.getBytes(StandardCharsets.UTF_8));
                Log.i(TAG_CONFIG, "Ensured Escape=ShowMenu binding: " + file.getAbsolutePath());
            }
        } catch (IOException ex) {
            Log.w(TAG_CONFIG, "Could not ensure menu key binding in " + file.getAbsolutePath() + ": " + ex);
        }
    }

    // UNREAL_ANDROID_CORE_CONFIG_FIX_V144: guarantee the engine-critical config keys the port depends on so a
    // stale or incomplete on-device Unreal.ini self-heals (copyAssetIfMissing never overwrites an
    // existing file, so a config written by an older/broken build would otherwise stay broken forever). These
    // keys are fixed for this port and are not user-tunable:
    //  - [URL] needs the full FURL field set. Without MapExt/Protocol/Port, FURL cannot parse the local startup
    //    map "Klingon.unr" as a map file and misreads "Klingon.unr/Index.unr" as a network host/map, so Browse
    //    takes the network path and returns failure -> fatal "Failed to enter Klingon.unr".
    //  - [Core.System] Paths are the package (.u/.unr/.utx/...) search paths. UE1 reads them as INDEXED keys
    //    (Paths[0]=...), not repeated "Paths=" lines; without the indexed form GSys->Paths is empty and no
    //    package (not even the boot map) can be found.
    private static void repairCoreConfig(File file) {
        if (file == null || !file.isFile()) return;
        try {
            String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            String p = text;
            // Engine subsystem classes the engine resolves via LoadClass("ini:Engine.Engine.X").
            // Missing any of these is fatal ("Can't find 'ini:Engine.Engine.X' in configuration file").
            p = setIniValue(p, "Engine.Engine", "Canvas", "Engine.Canvas");
            p = setIniValue(p, "Engine.Engine", "Input", "Engine.Input");
            p = setIniValue(p, "Engine.Engine", "Console", "Engine.Console");
            p = setIniValue(p, "Engine.Engine", "NetworkDevice", "IpDrv.TcpNetDriver");
            p = setIniValueIfAbsent(p, "Engine.Engine", "DefaultGame", "Klingons.SinglePlayer");
            p = setIniValueIfAbsent(p, "Engine.Engine", "DefaultServerGame", "Klingons.DeathMatchGame");
            // Player spawn resolves LoadClass("ini:DefaultPlayer.Class") from Unreal.ini (not User.ini).
            p = setIniValueIfAbsent(p, "DefaultPlayer", "Name", "Player");
            p = setIniValue(p, "DefaultPlayer", "Class", "Klingons.DMMale");
            p = setIniValueIfAbsent(p, "URL", "Protocol", "unreal");
            p = setIniValue(p, "URL", "MapExt", "unr");
            p = setIniValue(p, "URL", "SaveExt", "usa");
            p = setIniValue(p, "URL", "Port", "7777");
            p = setIniValueIfAbsent(p, "URL", "Map", "Index.unr");
            p = setIniValueIfAbsent(p, "URL", "LocalMap", "Klingon.unr");
            p = setIniValue(p, "Core.System", "Paths[0]", "../System/*.u");
            p = setIniValue(p, "Core.System", "Paths[1]", "../Maps/*.unr");
            p = setIniValue(p, "Core.System", "Paths[2]", "../Textures/*.utx");
            p = setIniValue(p, "Core.System", "Paths[3]", "../Sounds/*.uax");
            p = setIniValue(p, "Core.System", "Paths[4]", "../Music/*.umx");
            if (!p.equals(text)) {
                Files.write(file.toPath(), p.getBytes(StandardCharsets.UTF_8));
                Log.i(TAG_CONFIG, "Repaired core engine config (URL/Paths): " + file.getAbsolutePath());
            }
        } catch (IOException ex) {
            Log.w(TAG_CONFIG, "Could not repair core config in " + file.getAbsolutePath() + ": " + ex);
        }
    }

    // UNREAL_ANDROID_RENDER_MODULE_FIX_V143: the [Engine.Engine] Render key is the software render
    // MODULE (a URenderBase subclass — must be Render.Render), NOT the render DEVICE. A shipped config
    // wrongly set Render=VulkanDrv.VulkanRenderDevice; at startup UGameEngine::Init does
    // LoadClass(URenderBase, "ini:Engine.Engine.Render") and rejects it — crashing fresh installs with
    // "LoadClassMismatch: VulkanRenderDevice is not a child of RenderBase". (The render DEVICE is set
    // separately via GameRenderDevice/WindowedRenderDevice, which stay VulkanDrv.VulkanRenderDevice.)
    // The render module is fixed on this port and never user-tunable, so force-correct it on EVERY launch
    // — this also repairs devices that already copied the broken config, since copyAssetIfMissing never
    // overwrites an existing /sdcard Unreal.ini.
    private static void repairRenderModule(File file) {
        if (file == null || !file.isFile()) return;
        try {
            String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            String patched = setIniValue(text, "Engine.Engine", "Render", "Render.Render");
            if (!patched.equals(text)) {
                Files.write(file.toPath(), patched.getBytes(StandardCharsets.UTF_8));
                Log.i(TAG_CONFIG, "Repaired [Engine.Engine] Render module to Render.Render: " + file.getAbsolutePath());
            }
        } catch (IOException ex) {
            Log.w(TAG_CONFIG, "Could not repair render module in " + file.getAbsolutePath() + ": " + ex);
        }
    }

    private static void patchNsdlControllerDefaults(File file) {
        if (file == null || !file.isFile()) return;
        try {
            String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            String patched = text;
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "UseJoystick", "True");
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "AndroidNativeController", "True");
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "AndroidNativeDirectInput", "True"); // UNREAL_ANDROID_CONFIG_PRESERVE_V139
            // UNREAL_ANDROID_RIGHTSTICK_DEFAULT_SENSITIVITY_V126: this launch-time patcher is the CANONICAL
            // controller default, applied to Unreal.ini (the single engine config file). An earlier build
            // force-pinned RightStickScale at 1.00 — silently capping look sensitivity no matter what the ini
            // said. Seed sane sensitivity defaults but PRESERVE the player's edits (set-if-absent), so these
            // are live-tunable in Unreal.ini. 2.5 = a moderate default; tune up/down to taste.
            patched = setIniValueIfAbsent(patched, "NSDLDrv.NSDLClient", "AndroidNativeRightStickScale", "2.50");
            patched = setIniValueIfAbsent(patched, "NSDLDrv.NSDLClient", "AndroidNativeLeftStickScale", "2.50"); // ANDROID_LEFTSTICK_NATIVE_SENSITIVITY_V127
            patched = setIniValueIfAbsent(patched, "NSDLDrv.NSDLClient", "AndroidNativeRightStickSmoothing", "False"); // ANDROID_RIGHT_STICK_SMOOTHING_TOGGLE_V129
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "AndroidNativeLeftStickDeadzone", "0.08"); // UNREAL_ANDROID_CONFIG_PRESERVE_V139
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "AndroidNativeRightStickDeadzone", "0.10"); // UNREAL_ANDROID_CONFIG_PRESERVE_V139
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "AndroidNativeTriggerDeadzone", "0.12"); // UNREAL_ANDROID_CONFIG_PRESERVE_V139
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "AndroidNativeAxisCurve", "1.00"); // UNREAL_ANDROID_CONFIG_PRESERVE_V139
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "DeadZoneXYZ", "0.10");
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "DeadZoneRUV", "0.10");
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "ScaleXYZ", "100.0");
            patched = setIniValue(patched, "NSDLDrv.NSDLClient", "ScaleRUV", "100.0");
            if (!patched.equals(text)) {
                Files.write(file.toPath(), patched.getBytes(StandardCharsets.UTF_8));
                Log.i(TAG_CONFIG, "Patched Android controller defaults: " + file.getAbsolutePath());
            }
        } catch (IOException ex) {
            Log.w(TAG_CONFIG, "Could not patch controller defaults in " + file.getAbsolutePath() + ": " + ex);
        }
    }

    private static void appendControllerInputFallbacks(File file) {
        if (file == null) return;
        try {
            String text = file.isFile()
                    ? new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8)
                    : "[DefaultPlayer]\nName=Player\nClass=UnrealShare.MaleOne\n\n";
            if (text.contains("UNREAL_ANDROID_CONTROLLER_DIRECT_V122")
                    || hasAnyEngineInputBindings(text)
                    || hasAndroidControllerFallbackBindings(text)) {
                Log.i(TAG_CONFIG, "Preserved existing input bindings: " + file.getAbsolutePath()); // UNREAL_ANDROID_CONFIG_PRESERVE_V139
                return;
            }
            String block = "\n\n; UNREAL_ANDROID_CONTROLLER_DIRECT_V122 UNREAL_ANDROID_CONFIG_PRESERVE_V139\n" +
                    "; Robust Android controller fallbacks. Direct mode uses W/A/S/D + mouse buttons for gameplay,\n" +
                    "; while these Joy*/friendly aliases keep Customize Controls and SDL fallback usable.\n" +
                    "[Engine.Input]\n" +
                    "LeftMouse=Fire\n" +
                    "RightMouse=AltFire\n" +
                    "MouseX=Axis aMouseX Speed=6.0\n" +
                    "MouseY=Axis aMouseY Speed=6.0\n" +
                    "W=MoveForward\n" +
                    "S=MoveBackward\n" +
                    "A=StrafeLeft\n" +
                    "D=StrafeRight\n" +
                    "Space=Jump\n" +
                    "C=Duck\n" +
                    "G=Grab\n" +
                    "Joy1=Jump\n" +
                    "Joy2=Duck\n" +
                    "Joy3=Grab\n" +
                    "Joy4=Walking\n" +
                    "Joy5=ActivateTranslator\n" +
                    "Joy8=Duck\n" +
                    "Joy9=CenterView\n" +
                    "Joy10=PrevWeapon\n" +
                    "Joy11=NextWeapon\n" +
                    "Joy12=AltFire\n" +
                    "Joy13=Fire\n" +
                    "Joy14=TurnLeft\n" +
                    "Joy15=TurnRight\n" +
                    "Joy16=LookUp\n" +
                    "JoyX=Axis aStrafe Speed=1\n" +
                    "JoyY=Axis aBaseY Speed=1\n" +
                    "JoyU=Axis aTurn Speed=1\n" +
                    "JoyV=Axis aLookUp Speed=-1\n" +
                    "JoyPovRight=NextWeapon\n" +
                    "JoyPovLeft=PrevWeapon\n" +
                    "JoyPovUp=InventoryPrevious\n" +
                    "JoyPovDown=InventoryNext\n" +
                    "UnknownD8=StrafeLeft\n" +
                    "UnknownD9=StrafeRight\n" +
                    "UnknownDA=MoveForward\n" +
                    "UnknownDF=MoveBackward\n" +
                    "UnknownEA=LookDown\n";
            Files.write(file.toPath(), (text + block).getBytes(StandardCharsets.UTF_8));
            Log.i(TAG_CONFIG, "Appended Android controller input fallbacks: " + file.getAbsolutePath());
        } catch (IOException ex) {
            Log.w(TAG_CONFIG, "Could not append controller input fallbacks in " + file.getAbsolutePath() + ": " + ex);
        }
    }

    private static boolean hasAnyEngineInputBindings(String text) {
        // UNREAL_ANDROID_CONFIG_PRESERVE_V139
        // A retained install may already contain user-customized controls.  Do not
        // append a second default [Engine.Input] block because UE1 keeps the last
        // duplicate key and that would effectively reset the user's bindings.
        if (text == null) return false;
        String[] lines = text.replace("\r\n", "\n").replace('\r', '\n').split("\n");
        boolean inInput = false;
        for (String raw : lines) {
            String t = raw.trim();
            if (t.length() == 0 || t.startsWith(";") || t.startsWith("#")) continue;
            if (t.startsWith("[") && t.endsWith("]")) {
                inInput = t.equalsIgnoreCase("[Engine.Input]");
                continue;
            }
            if (inInput && t.indexOf('=') > 0) return true;
        }
        return false;
    }

    private static boolean hasAndroidControllerFallbackBindings(String text) {
        // UNREAL_ANDROID_CONFIG_PRESERVE_V139
        if (text == null) return false;
        return text.contains("UnknownD8=StrafeLeft")
                || text.contains("UnknownDA=MoveForward")
                || text.contains("Joy11=NextWeapon")
                || text.contains("Joy13=Fire");
    }

    private static String setIniValue(String text, String section, String key, String value) {
        if (text == null) text = "";
        String normalized = text.replace("\r\n", "\n").replace('\r', '\n');
        String sectionHeader = "[" + section + "]";
        java.util.regex.Pattern sectionPattern = java.util.regex.Pattern.compile("(?im)^\\[" + java.util.regex.Pattern.quote(section) + "\\]\\s*$");
        java.util.regex.Matcher sectionMatcher = sectionPattern.matcher(normalized);
        if (!sectionMatcher.find()) {
            if (normalized.length() > 0 && !normalized.endsWith("\n")) normalized += "\n";
            return normalized + "\n" + sectionHeader + "\n" + key + "=" + value + "\n";
        }

        int sectionStart = sectionMatcher.end();
        java.util.regex.Pattern nextSectionPattern = java.util.regex.Pattern.compile("(?m)^\\[[^\\]]+\\]\\s*$");
        java.util.regex.Matcher nextSectionMatcher = nextSectionPattern.matcher(normalized);
        int sectionEnd = normalized.length();
        while (nextSectionMatcher.find(sectionStart)) {
            sectionEnd = nextSectionMatcher.start();
            break;
        }

        String before = normalized.substring(0, sectionStart);
        String body = normalized.substring(sectionStart, sectionEnd);
        String after = normalized.substring(sectionEnd);
        java.util.regex.Pattern keyPattern = java.util.regex.Pattern.compile("(?im)^" + java.util.regex.Pattern.quote(key) + "\\s*=.*$");
        java.util.regex.Matcher keyMatcher = keyPattern.matcher(body);
        if (keyMatcher.find()) {
            body = keyMatcher.replaceAll(java.util.regex.Matcher.quoteReplacement(key + "=" + value));
        } else {
            if (!body.endsWith("\n")) body += "\n";
            body += key + "=" + value + "\n";
        }
        return before + body + after;
    }

    // UNREAL_ANDROID_RIGHTSTICK_DEFAULT_SENSITIVITY_V126: seed a default but PRESERVE a user-edited value.
    // Unlike setIniValue (which force-overwrites every launch), this only writes the key when it is missing,
    // so the player can tune sensitivity/smoothing in Unreal.ini and have it persist across launches.
    private static String setIniValueIfAbsent(String text, String section, String key, String value) {
        if (text == null) text = "";
        String normalized = text.replace("\r\n", "\n").replace('\r', '\n');
        java.util.regex.Matcher sectionMatcher = java.util.regex.Pattern
                .compile("(?im)^\\[" + java.util.regex.Pattern.quote(section) + "\\]\\s*$").matcher(normalized);
        if (sectionMatcher.find()) {
            int sectionStart = sectionMatcher.end();
            java.util.regex.Matcher nextSection = java.util.regex.Pattern.compile("(?m)^\\[[^\\]]+\\]\\s*$").matcher(normalized);
            int sectionEnd = nextSection.find(sectionStart) ? nextSection.start() : normalized.length();
            String body = normalized.substring(sectionStart, sectionEnd);
            if (java.util.regex.Pattern.compile("(?im)^" + java.util.regex.Pattern.quote(key) + "\\s*=.*$").matcher(body).find())
                return text; // already present -> keep the player's value
        }
        return setIniValue(text, section, key, value);
    }

    private static void ensureConfigFile(File systemDir, String targetName, String[] templateNames, String fallbackText) throws IOException {
        if (!systemDir.exists() && !systemDir.mkdirs()) Log.w(TAG_CONFIG, "Could not create System directory: " + systemDir.getAbsolutePath());
        final File target = new File(systemDir, targetName);
        if (target.exists()) {
            Log.i(TAG_CONFIG, targetName + " exists: " + target.getAbsolutePath());
            return;
        }
        for (String templateName : templateNames) {
            final File template = new File(systemDir, templateName);
            if (template.exists() && template.isFile()) {
                copyFile(template, target);
                Log.i(TAG_CONFIG, targetName + " created from " + templateName + ": " + target.getAbsolutePath());
                return;
            }
        }
        try (FileOutputStream out = new FileOutputStream(target)) {
            if (fallbackText != null && fallbackText.length() > 0) out.write(fallbackText.getBytes("UTF-8"));
            out.flush();
            Log.i(TAG_CONFIG, targetName + " created from fallback: " + target.getAbsolutePath());
        }
    }

    private static void copyFile(File src, File dst) throws IOException {
        File parent = dst.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) throw new IOException("Could not create " + parent.getAbsolutePath());
        try (FileInputStream in = new FileInputStream(src); FileOutputStream out = new FileOutputStream(dst)) {
            copyStream(in, out);
        }
    }

    private static void copyStream(InputStream in, FileOutputStream out) throws IOException {
        byte[] buffer = new byte[128 * 1024];
        int read;
        while ((read = in.read(buffer)) != -1) out.write(buffer, 0, read);
        out.flush();
    }

    static ImportResult importUnrealFolderFromSaf(Context context, Uri treeUri) {
        if (treeUri == null) return ImportResult.fail("No folder selected.");
        try {
            String selectedDocId = DocumentsContract.getTreeDocumentId(treeUri);
            if (selectedDocId == null || selectedDocId.length() == 0) return ImportResult.fail("The selected folder could not be read.");

            String unrealDocId = findSafUnrealRootDocId(context, treeUri, selectedDocId);
            if (unrealDocId == null) {
                return ImportResult.fail("The selected folder does not contain valid Klingon Honor Guard data. Please select the 'Unreal' folder. Expected at least: System/Core.u, System/Engine.u, System/Klingons.u, and Maps/*.unr.");
            }

            File target = primaryAppRoot(context);
            ensureDirectoryLayout(target);
            Log.i(TAG_IMPORT, "Importing SAF Unreal folder to " + target.getAbsolutePath());
            copySafTree(context, treeUri, unrealDocId, target);
            installDefaultConfigsIfNeeded(context, target);
            normalizeConfigForDetectedData(target);
            ensureAndroidControllerDirectPatch(new File(target, "System"));

            if (!hasRequiredData(target, true)) {
                return ImportResult.fail("The folder was copied, but required files are still missing in " + target.getAbsolutePath());
            }
            return ImportResult.ok(target, "Unreal data was imported successfully to:\n" + target.getAbsolutePath());
        } catch (Throwable t) {
            return ImportResult.fail("Import from the selected folder failed.", t);
        }
    }

    static ImportResult importUnrealZip(Context context, Uri zipUri) {
        if (zipUri == null) return ImportResult.fail("No ZIP file selected.");
        try {
            String rootPrefix = detectUnrealZipRootPrefix(context, zipUri);
            if (rootPrefix == null) {
                return ImportResult.fail("The ZIP file does not contain a valid Klingon Honor Guard data structure. Expected at least: System/Core.u, System/Engine.u, System/Klingons.u, and Maps/*.unr.");
            }

            File target = primaryAppRoot(context);
            ensureDirectoryLayout(target);
            Log.i(TAG_IMPORT, "Importing ZIP Unreal root prefix='" + rootPrefix + "' to " + target.getAbsolutePath());
            extractZipRoot(context, zipUri, rootPrefix, target);
            installDefaultConfigsIfNeeded(context, target);
            normalizeConfigForDetectedData(target);
            ensureAndroidControllerDirectPatch(new File(target, "System"));

            if (!hasRequiredData(target, true)) {
                return ImportResult.fail("The ZIP file was extracted, but required files are still missing in " + target.getAbsolutePath());
            }
            return ImportResult.ok(target, "Unreal data was imported successfully from the ZIP file to:\n" + target.getAbsolutePath());
        } catch (Throwable t) {
            return ImportResult.fail("Import from the ZIP file failed.", t);
        }
    }

    private static String findSafUnrealRootDocId(Context context, Uri treeUri, String selectedDocId) {
        if (safTreeHasRequiredData(context, treeUri, selectedDocId)) return selectedDocId;
        SafNode unrealChild = findSafChild(context, treeUri, selectedDocId, "Unreal", true);
        if (unrealChild != null && safTreeHasRequiredData(context, treeUri, unrealChild.docId)) return unrealChild.docId;
        return null;
    }

    private static boolean safTreeHasRequiredData(Context context, Uri treeUri, String rootDocId) {
        SafNode system = findSafChild(context, treeUri, rootDocId, "System", true);
        SafNode maps = findSafChild(context, treeUri, rootDocId, "Maps", true);
        if (system == null || maps == null) return false;
        boolean core = findSafChild(context, treeUri, system.docId, "Core.u", false) != null;
        boolean engine = findSafChild(context, treeUri, system.docId, "Engine.u", false) != null;
        boolean game = findSafChild(context, treeUri, system.docId, "Klingons.u", false) != null;
        boolean map = hasAnySafMap(context, treeUri, maps.docId);
        Log.i(TAG_IMPORT, "SAF data check: core=" + core + " engine=" + engine + " game=" + game + " map=" + map + " doc=" + rootDocId);
        return core && engine && game && map;
    }

    private static SafNode findSafChild(Context context, Uri treeUri, String parentDocId, String expectedName, boolean expectedDir) {
        for (SafNode child : listSafChildren(context, treeUri, parentDocId)) {
            if (!expectedName.equalsIgnoreCase(child.name)) continue;
            if (expectedDir && !child.isDirectory()) continue;
            if (!expectedDir && child.isDirectory()) continue;
            return child;
        }
        return null;
    }

    private static boolean hasAnySafMap(Context context, Uri treeUri, String mapsDocId) {
        for (SafNode child : listSafChildren(context, treeUri, mapsDocId)) {
            if (!child.isDirectory() && child.name != null && child.name.toLowerCase(Locale.ROOT).endsWith(".unr")) return true;
        }
        return false;
    }

    private static List<SafNode> listSafChildren(Context context, Uri treeUri, String parentDocId) {
        ArrayList<SafNode> out = new ArrayList<>();
        ContentResolver resolver = context.getContentResolver();
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocId);
        String[] projection = new String[] {
                Document.COLUMN_DOCUMENT_ID,
                Document.COLUMN_DISPLAY_NAME,
                Document.COLUMN_MIME_TYPE
        };
        try (Cursor cursor = resolver.query(childrenUri, projection, null, null, null)) {
            if (cursor == null) return out;
            while (cursor.moveToNext()) {
                String docId = cursor.getString(0);
                String name = cursor.getString(1);
                String mime = cursor.getString(2);
                if (docId == null || name == null) continue;
                out.add(new SafNode(docId, name, mime));
            }
        } catch (Throwable t) {
            Log.w(TAG_IMPORT, "Could not list SAF children for doc=" + parentDocId + ": " + t);
        }
        return out;
    }

    private static void copySafTree(Context context, Uri treeUri, String parentDocId, File outDir) throws IOException {
        if (!outDir.exists() && !outDir.mkdirs()) throw new IOException("Could not create " + outDir.getAbsolutePath());
        for (SafNode child : listSafChildren(context, treeUri, parentDocId)) {
            String safeName = sanitizeFileName(child.name);
            if (safeName.length() == 0) continue;
            File out = new File(outDir, safeName);
            if (child.isDirectory()) {
                copySafTree(context, treeUri, child.docId, out);
            } else {
                Uri fileUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, child.docId);
                File parent = out.getParentFile();
                if (parent != null && !parent.exists() && !parent.mkdirs()) throw new IOException("Could not create " + parent.getAbsolutePath());
                try (InputStream in = context.getContentResolver().openInputStream(fileUri); FileOutputStream fos = new FileOutputStream(out)) {
                    if (in == null) throw new IOException("Could not open SAF file " + child.name);
                    copyStream(in, fos);
                }
            }
        }
    }

    private static String sanitizeFileName(String name) {
        if (name == null) return "";
        return name.replace('/', '_').replace('\\', '_').trim();
    }

    private static String detectUnrealZipRootPrefix(Context context, Uri zipUri) throws IOException {
        HashMap<String, ZipRootFlags> roots = new HashMap<>();
        InputStream raw = context.getContentResolver().openInputStream(zipUri);
        if (raw == null) throw new IOException("Could not open ZIP stream");
        try (ZipInputStream zip = new ZipInputStream(raw)) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (entry.isDirectory()) continue;
                String normalized = normalizeZipName(entry.getName());
                if (normalized.length() == 0) continue;
                updateZipRootFlags(roots, normalized);
            }
        }

        String bestPrefix = null;
        int bestScore = -1;
        for (Map.Entry<String, ZipRootFlags> e : roots.entrySet()) {
            ZipRootFlags flags = e.getValue();
            if (!flags.valid()) continue;
            int score = flags.score();
            if (score > bestScore || (score == bestScore && (bestPrefix == null || e.getKey().length() < bestPrefix.length()))) {
                bestScore = score;
                bestPrefix = e.getKey();
            }
        }
        Log.i(TAG_IMPORT, "Detected ZIP Unreal root prefix: " + bestPrefix);
        return bestPrefix;
    }

    private static void updateZipRootFlags(HashMap<String, ZipRootFlags> roots, String normalizedName) {
        String[] parts = normalizedName.split("/");
        for (int i = 0; i < parts.length - 1; ++i) {
            String dir = parts[i];
            String file = parts[i + 1];
            String prefix = joinPrefix(parts, i);
            ZipRootFlags flags = roots.get(prefix);
            if (flags == null) {
                flags = new ZipRootFlags();
                roots.put(prefix, flags);
            }
            if ("System".equalsIgnoreCase(dir)) {
                if ("Core.u".equalsIgnoreCase(file)) flags.core = true;
                else if ("Engine.u".equalsIgnoreCase(file)) flags.engine = true;
                else if ("Klingons.u".equalsIgnoreCase(file)) flags.klingons = true;
            } else if ("Maps".equalsIgnoreCase(dir) && file.toLowerCase(Locale.ROOT).endsWith(".unr")) {
                flags.map = true;
            }
        }
    }

    private static String joinPrefix(String[] parts, int count) {
        if (count <= 0) return "";
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < count; ++i) {
            if (i > 0) b.append('/');
            b.append(parts[i]);
        }
        b.append('/');
        return b.toString();
    }

    private static void extractZipRoot(Context context, Uri zipUri, String rootPrefix, File targetRoot) throws IOException {
        String targetCanonical = targetRoot.getCanonicalPath() + File.separator;
        InputStream raw = context.getContentResolver().openInputStream(zipUri);
        if (raw == null) throw new IOException("Could not open ZIP stream");
        try (ZipInputStream zip = new ZipInputStream(raw)) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (entry.isDirectory()) continue;
                String normalized = normalizeZipName(entry.getName());
                if (normalized.length() == 0 || !normalized.startsWith(rootPrefix)) continue;
                String relative = normalized.substring(rootPrefix.length());
                if (relative.length() == 0 || relative.contains("../") || relative.startsWith("/")) continue;
                File out = new File(targetRoot, relative.replace('/', File.separatorChar));
                String outCanonical = out.getCanonicalPath();
                if (!outCanonical.startsWith(targetCanonical)) throw new IOException("Unsafe ZIP entry: " + entry.getName());
                File parent = out.getParentFile();
                if (parent != null && !parent.exists() && !parent.mkdirs()) throw new IOException("Could not create " + parent.getAbsolutePath());
                try (FileOutputStream fos = new FileOutputStream(out)) {
                    copyStream(zip, fos);
                }
            }
        }
    }

    private static String normalizeZipName(String name) {
        if (name == null) return "";
        String s = name.replace('\\', '/');
        while (s.startsWith("/")) s = s.substring(1);
        while (s.contains("//")) s = s.replace("//", "/");
        if (s.contains("../") || s.equals("..")) return "";
        return s;
    }

    static String candidateDescription(Context context) {
        StringBuilder b = new StringBuilder();
        for (File candidate : candidateRoots(context)) b.append("\n- ").append(candidate.getAbsolutePath());
        return b.toString();
    }
}
