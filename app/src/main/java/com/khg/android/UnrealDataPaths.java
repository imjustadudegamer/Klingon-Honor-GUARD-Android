package com.khg.android;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
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
        // UNREAL_ANDROID_OBB_ROOT_V151: consolidate ALL game data, save games, and config under the
        // app's OBB directory (Android/obb/<pkg>/Unreal). This is app-private (needs no storage
        // permission on any Android version), deterministic (no more scanning arbitrary SD-card /
        // volume locations), and is the single root that BOTH the Java bootstrap and the native
        // engine agree on. getObbDir() may return null on exotic setups, so fall back to the old
        // app-specific external dir, then internal storage.
        File base = context.getObbDir();
        if (base == null) base = context.getExternalFilesDir(null);
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
        // UNREAL_ANDROID_OBB_ROOT_V151: OBB is the ONE and ONLY data root. The old broad scan of
        // /sdcard/Unreal, every /storage/* and /mnt/* volume, and each external-files dir was
        // removed. That scan caused the game to "randomly" pick up data from an arbitrary volume
        // and — worse — could diverge from the root the native engine independently chose (config
        // written to root A, engine boots root B), which surfaces at startup as
        // Core.Errors.ConfigNotFound / Engine.Errors.LoadEntry. There is now exactly one candidate.
        ArrayList<File> out = new ArrayList<>();
        HashSet<String> seen = new HashSet<>();
        addCandidate(out, seen, primaryAppRoot(context));
        return out;
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
        // UNREAL_ANDROID_SINGLE_INI_V151: Unreal.ini is the ONE AND ONLY config file. It is the DefaultIni
        // the engine loads AND saves (UnConfig.cpp/UnPlat.cpp, Filename=NULL). We used to also ship/copy
        // User.ini (an inert stub the engine never loads — verified: zero engine references),
        // AndroidController.ini (pure comments, read by nothing), and AndroidUI.ini (only its UIScale key
        // was read, now folded into Unreal.ini's [Unreal.UnrealOptionsMenu]). All three are gone so there
        // are no stray, unused .ini files created or loaded at runtime — one file, one source of truth.
        copyAssetIfMissing(context, "ue1_config/Unreal.ini", new File(systemDir, "Unreal.ini"));
        // UNREAL_ANDROID_CONFIG_CORRUPTION_GUARD_V152: if the on-device Unreal.ini exists but is genuinely
        // corrupt (binary garbage, NUL bytes, or a non-ini blob), back it up and rewrite from the bundled
        // template BEFORE the section-merge/key-repair below, so those operate on a valid file.
        healCorruptUnrealIni(context, new File(systemDir, "Unreal.ini"), "ue1_config/Unreal.ini");
        // UNREAL_ANDROID_CONFIG_WRITABLE_GUARD_V152: a valid Unreal.ini placed by something other than the
        // app (an adb push — owned by shell; a file-manager copy; a backup/restore) can belong to a
        // different uid and be read-only to the app, so every later repair AND the engine's own SaveConfig
        // silently fail with EACCES. Reclaim ownership in place (content preserved) so all downstream writes
        // — Java and native — succeed. Runs after the corruption guard so a corrupt file is already gone.
        ensureConfigWritable(new File(systemDir, "Unreal.ini"));
        // UNREAL_ANDROID_CONFIG_HARDEN_V151: copyAssetIfMissing only writes when the file is ABSENT.
        // A truncated or partially-written Unreal.ini (an interrupted first-run copy, a file left by
        // an older/broken build, or one native accidentally stubbed) is otherwise kept as-is and then
        // fails deep in boot with Core.Errors.ConfigNotFound / Engine.Errors.LoadEntry because whole
        // engine/game sections are missing. Heal it in place: write the full template if it is
        // missing/empty, otherwise merge back any [Section] that the shipped asset has but the
        // on-device file lacks. Existing sections/keys (including the player's rebinds) are untouched.
        ensureUnrealIniComplete(context, new File(systemDir, "Unreal.ini"), "ue1_config/Unreal.ini");
    }

    // UNREAL_ANDROID_CONFIG_CORRUPTION_GUARD_V152: full-recovery safety net. copyAssetIfMissing only writes
    // when the file is ABSENT and ensureUnrealIniComplete only merges MISSING sections — neither repairs a
    // file that exists but is genuinely corrupt (binary garbage from an interrupted write, a NUL-filled blob,
    // a non-ini file). Detect that and, if corrupt, preserve it as Unreal.ini.corrupt.bak (never destroyed)
    // and rewrite Unreal.ini from the bundled template. Runs before the section/key repair so those see a
    // valid file; reconstructSaveSlots afterwards restores the save-slot labels from Save/*.usa.
    // UNREAL_ANDROID_CONFIG_CRITICAL_DATA_GUARD_V153: also treat as corrupt a file that passes the
    // binary-garbage test but is missing boot-critical sections/keys (see configMissingCriticalData) — the
    // common real-world case being a config truncated mid-section, which isIniCorrupt misses (it still has a
    // valid [header]) and the section-merge misses (that section's header is present, so it's never re-added).
    private static void healCorruptUnrealIni(Context context, File target, String asset) {
        try {
            if (target == null || !target.isFile()) return; // absence is handled by copyAssetIfMissing
            byte[] bytes = Files.readAllBytes(target.toPath());
            String reason;
            if (isIniCorrupt(bytes)) {
                reason = "binary/non-ini content";
            } else {
                String foreign = configForeignPlatform(bytes);
                String missing = configMissingCriticalData(bytes);
                if (foreign != null) {
                    reason = "foreign-platform config (" + foreign + ")"; // e.g. a Windows Unreal.ini
                } else if (missing != null) {
                    reason = "missing critical " + missing;
                } else {
                    return; // valid, boot-complete Android config — leave it (and its user edits) alone
                }
            }
            File backup = new File(target.getParentFile(), target.getName() + ".corrupt.bak");
            boolean backedUp = false;
            try {
                Files.copy(target.toPath(), backup.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                backedUp = true;
            } catch (Throwable ignore) { /* best-effort backup; can fail when the file was placed by another uid
                                           (e.g. adb push / a file manager owns System/) — still rewrite below */ }
            try (InputStream in = context.getAssets().open(asset)) {
                byte[] template = readAllBytes(in);
                if (template.length == 0) return; // asset unreadable — do not destroy what little we have
                // Overwrite in place: Files.write TRUNCATEs the existing inode, which succeeds even when the
                // file was placed by another uid and we cannot unlink it. (delete() is a best-effort tidy-up
                // that no longer gates the rewrite; a failed delete previously left a misleading "backed up" log.)
                try { target.delete(); } catch (Throwable ignore) { /* fall through to overwrite */ }
                Files.write(target.toPath(), template);
            }
            Log.w(TAG_CONFIG, "Unreal.ini failed integrity check (" + reason + "); "
                    + (backedUp ? "backed up to " + backup.getName() + " and " : "could not back up (proceeding), ")
                    + "rewrote from template: " + target.getAbsolutePath());
        } catch (Throwable t) {
            Log.w(TAG_CONFIG, "Could not verify/heal Unreal.ini integrity for "
                    + (target != null ? target.getAbsolutePath() : "null") + ": " + t);
        }
    }

    // UNREAL_ANDROID_CONFIG_WRITABLE_GUARD_V152: make Unreal.ini writable by the app regardless of who
    // created it. A file pushed/restored by another uid (adb=shell, a backup app, a file manager) can be
    // read-only to the app; since the app owns the System/ directory it can always replace files in it.
    // If the config exists but is not app-writable, rewrite it in place with the SAME content so it becomes
    // app-owned — content preserved, only ownership/permission fixed. No-op when already writable.
    private static void ensureConfigWritable(File target) {
        try {
            if (target == null || !target.isFile()) return;
            if (target.canWrite()) return; // already writable by the app — nothing to do
            byte[] content = Files.readAllBytes(target.toPath());
            target.delete(); // app owns the dir -> can remove a foreign-owned/read-only file
            Files.write(target.toPath(), content);
            Log.w(TAG_CONFIG, "Unreal.ini was not app-writable; rewrote in place to reclaim ownership: "
                    + target.getAbsolutePath());
        } catch (Throwable t) {
            Log.w(TAG_CONFIG, "Could not make Unreal.ini app-writable: " + t);
        }
    }

    // True only when the bytes are clearly not a usable text ini: a NUL byte, too many non-text control
    // bytes (binary garbage), or non-empty with no [section] header at all. NOTE: UE1 configs are ANSI/
    // Latin-1, so high bytes (0x80-0xFF, e.g. an accented player name) are NOT treated as corruption — a
    // strict-UTF-8 test would wrongly nuke valid configs. Empty/whitespace files are NOT reported here;
    // ensureUnrealIniComplete already rewrites those from the template.
    private static boolean isIniCorrupt(byte[] bytes) {
        if (bytes == null || bytes.length == 0) return false;
        int control = 0;
        for (byte b : bytes) {
            int u = b & 0xFF;
            if (u == 0) return true; // NUL byte => binary garbage, never in a text ini
            if (u < 0x09 || (u > 0x0D && u < 0x20)) control++; // control chars other than \t \n \v \f \r
        }
        if ((long) control * 20 > bytes.length) return true; // >5% control bytes => not text
        // ISO-8859-1 keeps every byte 1:1 (ASCII section headers survive); a non-empty file with no
        // [section] header at all is not a usable ini.
        String text = new String(bytes, StandardCharsets.ISO_8859_1);
        if (text.trim().length() > 0
                && !java.util.regex.Pattern.compile("(?m)^\\s*\\[[^\\]]+\\]\\s*$").matcher(text).find())
            return true;
        return false;
    }

    // UNREAL_ANDROID_CONFIG_CRITICAL_DATA_GUARD_V153: {section, key} pairs the engine cannot boot without.
    // Each MUST be present-and-non-empty in every valid Unreal.ini the engine writes, so a personalized but
    // healthy config never trips this — only a truncated/gutted one does. Keep this list MINIMAL and truly
    // boot-critical (absence here is what produced Core.Errors.ConfigNotFound / Engine.Errors.LoadEntry):
    // the render device + game engine class, the boot map + protocol, and the package search paths.
    private static final String[][] REQUIRED_CONFIG_DATA = {
        { "Engine.Engine", "GameRenderDevice" },
        { "Engine.Engine", "GameEngine" },
        { "URL",           "Protocol" },
        { "URL",           "LocalMap" },
        { "Core.System",   "Paths[0]" },
        { "Core.System",   "SavePath" },
    };

    // Returns "Section/Key" for the first REQUIRED_CONFIG_DATA entry that is missing or blank, or null when
    // all critical data is present. Decoded ISO-8859-1 to match isIniCorrupt (UE1 configs are ANSI/Latin-1,
    // so a high byte such as an accented player name never derails the parse). getIniValue returns null for
    // an absent section/key and "" for a present-but-empty one; both count as missing for these keys, which
    // are never legitimately blank — that is exactly how a value truncated to "GameRenderDevice=" is caught.
    private static String configMissingCriticalData(byte[] bytes) {
        if (bytes == null || bytes.length == 0) return "Engine.Engine/GameRenderDevice";
        String text = new String(bytes, StandardCharsets.ISO_8859_1);
        for (String[] req : REQUIRED_CONFIG_DATA) {
            String v = getIniValue(text, req[0], req[1]);
            if (v == null || v.length() == 0) return req[0] + "/" + req[1];
        }
        return null;
    }

    // UNREAL_ANDROID_CONFIG_FOREIGN_GUARD_V153: a Unreal.ini copied from a Windows (or any non-Android) install
    // is a *valid* ini that passes the corruption and critical-data checks, but it names drivers this port does
    // not have — e.g. GameRenderDevice=GlideDrv/D3DDrv/SoftDrv/OpenGLDrv, ViewportManager=WinDrv.WindowsClient.
    // It cannot boot (the engine would try to bind GlideDrv.dll and fail). This port is Vulkan-only with the SDL
    // client, so the ONLY valid values are VulkanDrv.VulkanRenderDevice and NSDLDrv.NSDLClient; any other named
    // driver means the file belongs to another platform and must be rewritten from the bundled Android template.
    // Returns the offending "key=value", or null when the config is native (or the keys are absent — that case
    // is the critical-data guard's). Case-insensitive; getIniValue already trims. This is the specific reason
    // the user reported: dropping a retail Windows game folder onto the device left its Unreal.ini in place.
    private static String configForeignPlatform(byte[] bytes) {
        if (bytes == null || bytes.length == 0) return null;
        String text = new String(bytes, StandardCharsets.ISO_8859_1);
        String grd = getIniValue(text, "Engine.Engine", "GameRenderDevice");
        if (grd != null && grd.length() > 0 && !grd.equalsIgnoreCase("VulkanDrv.VulkanRenderDevice"))
            return "GameRenderDevice=" + grd;
        String vm = getIniValue(text, "Engine.Engine", "ViewportManager");
        if (vm != null && vm.length() > 0 && !vm.equalsIgnoreCase("NSDLDrv.NSDLClient"))
            return "ViewportManager=" + vm;
        return null;
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

    // UNREAL_ANDROID_CONFIG_HARDEN_V151: guarantee a COMPLETE Unreal.ini at the engine root.
    // - Missing or effectively empty file  -> write the full shipped template.
    // - Present but incomplete             -> append every [Section] the asset has that the on-device
    //                                         file lacks (merge; never rewrites existing sections).
    // This is the force-creation hardening: a partial config no longer boots into ConfigNotFound.
    private static void ensureUnrealIniComplete(Context context, File target, String asset) {
        try {
            String template;
            try (InputStream in = context.getAssets().open(asset)) {
                template = new String(readAllBytes(in), StandardCharsets.UTF_8);
            }
            if (template.trim().length() == 0) return; // asset unreadable/empty — nothing to enforce

            String current = target.isFile()
                    ? new String(Files.readAllBytes(target.toPath()), StandardCharsets.UTF_8)
                    : "";
            if (current.trim().length() == 0) {
                Files.write(target.toPath(), template.getBytes(StandardCharsets.UTF_8));
                Log.w(TAG_CONFIG, "Unreal.ini was missing/empty; wrote full template: " + target.getAbsolutePath());
                return;
            }

            HashSet<String> present = sectionHeadersLower(current);
            Map<String, String> templateSections = sectionBlocks(template);
            StringBuilder merged = new StringBuilder(current);
            int healed = 0;
            for (Map.Entry<String, String> e : templateSections.entrySet()) {
                if (present.contains(e.getKey())) continue;
                if (merged.length() > 0 && merged.charAt(merged.length() - 1) != '\n') merged.append('\n');
                merged.append('\n').append(e.getValue());
                healed++;
            }
            if (healed > 0) {
                Files.write(target.toPath(), merged.toString().getBytes(StandardCharsets.UTF_8));
                Log.w(TAG_CONFIG, "Healed incomplete Unreal.ini: merged " + healed
                        + " missing section(s): " + target.getAbsolutePath());
            }
        } catch (Throwable t) {
            Log.w(TAG_CONFIG, "Could not verify/heal Unreal.ini completeness for " + target.getAbsolutePath() + ": " + t);
        }
    }

    // UNREAL_ANDROID_DIFFICULTY_RESET_V095: the port historically shipped [Engine.GameInfo] Difficulty=3
    // (the hardest of four levels, inherited from a played-on-hard retail Unreal.ini). Difficulty is a
    // globalconfig byte, so it persists in Unreal.ini and is NOT stored per save. This does a ONE-TIME reset
    // to Normal (1) when a device first launches this release, so anyone stuck on the inherited hard default
    // is moved off it. It runs exactly once (guarded by an app-private flag), so it never fights a player's
    // own in-game difficulty choice on later launches. Fresh installs already ship Difficulty=1, so this is
    // a no-op for them beyond setting the flag.
    private static void oneTimeDifficultyResetV095(Context context, File systemDir) {
        try {
            if (context == null || systemDir == null) return;
            android.content.SharedPreferences prefs = context.getSharedPreferences("khg_bootstrap", Context.MODE_PRIVATE);
            final String KEY = "difficultyResetV095Done";
            if (prefs.getBoolean(KEY, false)) return; // already done on this install
            File ini = new File(systemDir, "Unreal.ini");
            if (ini.isFile()) {
                String text = new String(Files.readAllBytes(ini.toPath()), StandardCharsets.UTF_8);
                String updated = setIniValue(text, "Engine.GameInfo", "Difficulty", "1");
                if (!updated.equals(text)) {
                    Files.write(ini.toPath(), updated.getBytes(StandardCharsets.UTF_8));
                    Log.i(TAG_CONFIG, "One-time difficulty reset to Normal (1): " + ini.getAbsolutePath());
                }
            }
            prefs.edit().putBoolean(KEY, true).apply();
        } catch (Throwable t) {
            Log.w(TAG_CONFIG, "One-time difficulty reset failed: " + t);
        }
    }

    // UNREAL_ANDROID_TOUCHLOOK_FIX_V096: one-time config fix for existing installs. Two parts:
    // (1) Clear bAlwaysMouseLook so touch/right-stick vertical look works. KHG's aim code gates on
    //     "bAlwaysMouseLook", and this port historically shipped bAlwaysMouseLook=True; we now ship it False
    //     and hold the free-look modifier bLook natively so vertical touch-look is unchanged (see UnLevTic.cpp
    //     UNREAL_ANDROID_AUTOAIM_HOLD_BLOOK).
    // (2) Force MyAutoAim=1 (auto-aim OFF). Retail defaults auto-aim ON; we default it OFF. The in-menu
    //     "AUTO AIM" switch (OPTIONS MENU item 1) toggles [Engine.PlayerPawn] MyAutoAim between 1 (off) and
    //     0.93 (on), so this only sets the default; a later in-menu toggle still wins and persists.
    // Guarded by an app-private flag so it runs once. The flag is versioned (…OffV096) so devices that got an
    // earlier build's auto-aim-ON rewrite are corrected back to OFF exactly once. Both keys live in
    // [Engine.PlayerPawn]; setIniValue updates them in place (or adds them if somehow absent).
    private static void oneTimeTouchLookFixV096(Context context, File systemDir) {
        try {
            if (context == null || systemDir == null) return;
            android.content.SharedPreferences prefs = context.getSharedPreferences("khg_bootstrap", Context.MODE_PRIVATE);
            final String KEY = "touchLookFixAutoAimOffV096Done";
            if (prefs.getBoolean(KEY, false)) return; // already done on this install
            File ini = new File(systemDir, "Unreal.ini");
            if (ini.isFile()) {
                String text = new String(Files.readAllBytes(ini.toPath()), StandardCharsets.UTF_8);
                String updated = setIniValue(text, "Engine.PlayerPawn", "bAlwaysMouseLook", "False");
                updated = setIniValue(updated, "Engine.PlayerPawn", "MyAutoAim", "1.000000");
                if (!updated.equals(text)) {
                    Files.write(ini.toPath(), updated.getBytes(StandardCharsets.UTF_8));
                    Log.i(TAG_CONFIG, "One-time touch-look fix (bAlwaysMouseLook=False, auto-aim OFF MyAutoAim=1): " + ini.getAbsolutePath());
                }
            }
            prefs.edit().putBoolean(KEY, true).apply();
        } catch (Throwable t) {
            Log.w(TAG_CONFIG, "One-time touch-look fix failed: " + t);
        }
    }

    private static byte[] readAllBytes(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[16 * 1024];
        int read;
        while ((read = in.read(buf)) >= 0) bos.write(buf, 0, read);
        return bos.toByteArray();
    }

    // Set of lowercased section names ("engine.engine", "core.system", ...) present in an ini text.
    private static HashSet<String> sectionHeadersLower(String text) {
        HashSet<String> out = new HashSet<>();
        if (text == null) return out;
        for (String raw : text.replace("\r\n", "\n").replace('\r', '\n').split("\n")) {
            String t = raw.trim();
            if (t.length() >= 2 && t.charAt(0) == '[' && t.charAt(t.length() - 1) == ']') {
                out.add(t.substring(1, t.length() - 1).trim().toLowerCase(Locale.ROOT));
            }
        }
        return out;
    }

    // Ordered map lowercased-section-name -> full block text (header line through the line before the
    // next header). Preserves the asset's section order so merged-in sections read naturally.
    private static Map<String, String> sectionBlocks(String text) {
        java.util.LinkedHashMap<String, String> out = new java.util.LinkedHashMap<>();
        if (text == null) return out;
        String[] lines = text.replace("\r\n", "\n").replace('\r', '\n').split("\n", -1);
        String currentKey = null;
        StringBuilder block = null;
        for (String line : lines) {
            String t = line.trim();
            boolean isHeader = t.length() >= 2 && t.charAt(0) == '[' && t.charAt(t.length() - 1) == ']';
            if (isHeader) {
                if (currentKey != null) out.put(currentKey, block.toString());
                currentKey = t.substring(1, t.length() - 1).trim().toLowerCase(Locale.ROOT);
                block = new StringBuilder();
                block.append(line).append('\n');
            } else if (currentKey != null) {
                block.append(line).append('\n');
            }
        }
        if (currentKey != null) out.put(currentKey, block.toString());
        return out;
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
            migrateLegacySavesToObb(context, root);
            installDefaultConfigsIfNeeded(context, root);
            File systemDir = new File(root, "System");
            // UNREAL_ANDROID_SINGLE_INI_V151: Unreal.ini is the only config file — no User.ini stub is
            // seeded anymore (the engine never loads it).
            ensureConfigFile(systemDir, "Unreal.ini", new String[] { "Unreal.ini.default" }, "");
            ensureAndroidControllerDirectPatch(systemDir);
            removeObsoleteStrayInis(systemDir);
            oneTimeDifficultyResetV095(context, systemDir);
            oneTimeTouchLookFixV096(context, systemDir);
            reconstructSaveSlots(root);
            Log.i(TAG_CONFIG, "Config root: " + root.getAbsolutePath());
            Log.i(TAG_CONFIG, "Config file: " + new File(systemDir, "Unreal.ini").getAbsolutePath());
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
            patched = setIniValueIfAbsent(patched, "NSDLDrv.NSDLClient", "AndroidNativeRightStickScale", "2.00");
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

    // UNREAL_ANDROID_SINGLE_INI_CLEANUP_V152: Unreal.ini is the ONE and ONLY config file (V151). Older
    // builds shipped/synthesized AndroidController.ini (pure comments, read by nothing) and AndroidUI.ini
    // (its only key, UIScale, is now read from Unreal.ini's [Unreal.UnrealOptionsMenu]); User.ini/Default.ini
    // were inert stubs the engine never loads for game config. They can linger in System/ from a pre-V151
    // install. Delete these known-obsolete stray .ini files so the layout matches the single-ini design and
    // nothing stale can ever be read. Unreal.ini is never touched. Verified unused: no native/Java runtime
    // reader references any of these (UnCanvas.cpp:48 reads UIScale from Unreal.ini).
    private static final String[] OBSOLETE_STRAY_INIS = { "AndroidController.ini", "AndroidUI.ini", "User.ini", "Default.ini" };
    private static void removeObsoleteStrayInis(File systemDir) {
        if (systemDir == null || !systemDir.isDirectory()) return;
        for (String name : OBSOLETE_STRAY_INIS) {
            try {
                File stray = findCaseInsensitive(systemDir, name);
                if (stray != null && stray.isFile() && stray.delete())
                    Log.i(TAG_CONFIG, "Removed obsolete stray ini: " + stray.getAbsolutePath());
            } catch (Throwable t) {
                Log.w(TAG_CONFIG, "Could not remove stray ini " + name + ": " + t);
            }
        }
    }

    // Read a single ini value; returns null when the section or key is absent.
    private static String getIniValue(String text, String section, String key) {
        if (text == null) return null;
        String normalized = text.replace("\r\n", "\n").replace('\r', '\n');
        java.util.regex.Matcher sectionMatcher = java.util.regex.Pattern
                .compile("(?im)^\\[" + java.util.regex.Pattern.quote(section) + "\\]\\s*$").matcher(normalized);
        if (!sectionMatcher.find()) return null;
        int sectionStart = sectionMatcher.end();
        java.util.regex.Matcher nextSection = java.util.regex.Pattern.compile("(?m)^\\[[^\\]]+\\]\\s*$").matcher(normalized);
        int sectionEnd = nextSection.find(sectionStart) ? nextSection.start() : normalized.length();
        String body = normalized.substring(sectionStart, sectionEnd);
        java.util.regex.Matcher keyMatcher = java.util.regex.Pattern
                .compile("(?im)^" + java.util.regex.Pattern.quote(key) + "\\s*=(.*)$").matcher(body);
        return keyMatcher.find() ? keyMatcher.group(1).trim() : null;
    }

    // UNREAL_ANDROID_SAVE_SLOT_RECONSTRUCT_V152: KHG stores the load-menu save-slot labels as a
    // globalconfig array — [Klingons.KlingonMenuSlot] SlotNames[0..8] in Unreal.ini — NOT by scanning
    // the Save/ directory. So the menu lists a slot only if its config label is non-empty. Any event
    // that resets the config (the shipped Unreal.ini ships every SlotNames[k]=..Empty.., a reinstall, a
    // manual ini delete, or the V151 OBB save auto-import which copies *.usa but carries no config) leaves
    // the Save<k>.usa files physically present but UNLISTABLE — the player sees an empty Load menu even
    // though their saves are intact. Reliability fix: every launch, for each Save<k>.usa that exists, if
    // its SlotNames[k] is missing/blank/"..Empty..", seed a non-empty label (from the file's mtime) so the
    // slot lists and loads (?load=k). Purely additive — a real label written by the game is never touched,
    // and no save file is read or modified. k is a single digit 0-8: KHG hub sub-level saves are
    // Save<k><i>.usa (two digits) and are correctly ignored as non-slot files.
    private static void reconstructSaveSlots(File root) {
        try {
            if (root == null) return;
            File iniFile = new File(root, "System/Unreal.ini");
            if (!iniFile.isFile()) return;
            File saveDir = new File(root, "Save");
            if (!saveDir.isDirectory()) return;

            String text = new String(Files.readAllBytes(iniFile.toPath()), StandardCharsets.UTF_8);
            String updated = text;
            int recovered = 0;
            java.text.SimpleDateFormat fmt = new java.text.SimpleDateFormat("MM/dd/yy HH:mm", Locale.US);
            for (int k = 0; k <= 8; k++) {
                File save = new File(saveDir, "Save" + k + ".usa");
                if (!save.isFile()) continue;
                String cur = getIniValue(updated, "Klingons.KlingonMenuSlot", "SlotNames[" + k + "]");
                boolean empty = cur == null || cur.isEmpty() || cur.equalsIgnoreCase("..Empty..");
                if (!empty) continue; // a real label from the game — leave it alone
                String label = "Saved Game " + fmt.format(new java.util.Date(save.lastModified()));
                updated = setIniValue(updated, "Klingons.KlingonMenuSlot", "SlotNames[" + k + "]", label);
                recovered++;
            }
            if (!updated.equals(text)) {
                Files.write(iniFile.toPath(), updated.getBytes(StandardCharsets.UTF_8));
                Log.i(TAG_CONFIG, "Reconstructed " + recovered + " save-slot label(s) from Save/*.usa: " + iniFile.getAbsolutePath());
            }
        } catch (Throwable t) {
            Log.w(TAG_CONFIG, "Could not reconstruct save-slot labels: " + t);
        }
    }

    // UNREAL_ANDROID_OBB_SAVE_MIGRATION_V151: one-time, non-destructive rescue of on-device save games.
    // Before V151 the data root was getExternalFilesDir()/Unreal, so a returning player's *.usa saves
    // live in that OLD Save/ dir. The new OBB build won't read there, and SAF can't pick Android/data to
    // re-import them, so we auto-copy them once: if the OBB Save/ has no saves yet, copy every file from
    // the first legacy Save/ dir that has any. The old files are LEFT IN PLACE (never deleted). Game data
    // still comes via the normal import; only saves are migrated.
    static void migrateLegacySavesToObb(Context context, File obbRoot) {
        try {
            if (context == null || obbRoot == null) return;
            File targetSave = new File(obbRoot, "Save");
            if (hasAnySaveFile(targetSave)) return; // OBB already has saves — never overwrite

            ArrayList<File> legacySaveDirs = new ArrayList<>();
            File ext = context.getExternalFilesDir(null);
            if (ext != null) legacySaveDirs.add(new File(ext, "Unreal/Save"));
            File internal = context.getFilesDir();
            if (internal != null) legacySaveDirs.add(new File(internal, "Unreal/Save"));

            String targetCanonical;
            try { targetCanonical = targetSave.getCanonicalPath(); } catch (IOException e) { targetCanonical = targetSave.getAbsolutePath(); }

            for (File src : legacySaveDirs) {
                if (src == null || !src.isDirectory()) continue;
                String srcCanonical;
                try { srcCanonical = src.getCanonicalPath(); } catch (IOException e) { srcCanonical = src.getAbsolutePath(); }
                if (srcCanonical.equals(targetCanonical)) continue; // OBB fell back to the same dir — nothing to do
                if (!hasAnySaveFile(src)) continue;

                int copied = copyFlatFiles(src, targetSave);
                if (copied > 0) {
                    Log.i(TAG_STARTUP, "Auto-imported " + copied + " legacy save file(s): "
                            + src.getAbsolutePath() + " -> " + targetSave.getAbsolutePath());
                    return; // migrate from the first legacy dir that had saves
                }
            }
        } catch (Throwable t) {
            Log.w(TAG_STARTUP, "Legacy save auto-import failed: " + t);
        }
    }

    private static boolean hasAnySaveFile(File saveDir) {
        if (saveDir == null || !saveDir.isDirectory()) return false;
        File[] files = saveDir.listFiles((d, name) -> name.toLowerCase(Locale.ROOT).endsWith(".usa"));
        return files != null && files.length > 0;
    }

    private static int copyFlatFiles(File srcDir, File dstDir) {
        File[] files = srcDir.listFiles();
        if (files == null) return 0;
        if (!dstDir.exists() && !dstDir.mkdirs()) {
            Log.w(TAG_STARTUP, "Could not create OBB Save dir: " + dstDir.getAbsolutePath());
            return 0;
        }
        int copied = 0;
        for (File f : files) {
            if (f == null || !f.isFile()) continue; // UE1 saves are flat files; skip any subdirs
            File out = new File(dstDir, f.getName());
            if (out.exists()) continue; // never clobber an existing OBB save
            try {
                copyFile(f, out);
                copied++;
            } catch (IOException ex) {
                Log.w(TAG_STARTUP, "Could not migrate save " + f.getAbsolutePath() + ": " + ex);
            }
        }
        return copied;
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
}
