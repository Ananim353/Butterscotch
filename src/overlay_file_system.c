#include "overlay_file_system.h"
#include "utils.h"
#include "stb_ds.h"

#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define overlayMkdir(path) _mkdir(path)
#define overlayRmdir(path) _rmdir(path)
#else
#include <unistd.h>
#include <dirent.h>
#define overlayMkdir(path) mkdir((path), 0777)
#define overlayRmdir(path) rmdir(path)
#endif

#if defined(_MSC_VER) && !defined(S_ISDIR)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

// ===[ Helpers ]===

// Replaces all backslashes (\) with forward slashes (/) in a path string. (dealing with windows paths)
static inline char* normalizePath(const char* path) {
    if (path == nullptr) return nullptr;
    char* normalized = safeStrdup(path);
    for (int i = 0; normalized[i] != '\0'; i++) {
        if (normalized[i] == '\\') {
            normalized[i] = '/';
        }
    }
    return normalized;
}

static bool isAbsolute(const char* path) {
    return path != nullptr && path[0] == '/';
}

// Joins basePath (assumed to end with '/') with a relative path. Absolute inputs pass through as-is.
// Returns a heap-allocated string the caller must free.
static char* joinPath(const char* basePath, const char* normalizedPath) {
    if (isAbsolute(normalizedPath)) {
        // Absolute paths are passed through verbatim.
        return safeStrdup(normalizedPath);
    }
    size_t baseLen = strlen(basePath);
    size_t relLen = strlen(normalizedPath);
    char* full = (char *)safeMalloc(baseLen + relLen + 1);
    memcpy(full, basePath, baseLen);
    memcpy(full + baseLen, normalizedPath, relLen);
    full[baseLen + relLen] = '\0';
    return full;
}

static bool pathExists(const char* fullPath) {
    struct stat st;
    return stat(fullPath, &st) == 0;
}

// Returns a heap-allocated full path for reads. Absolute inputs pass through as-is.
// For relative inputs, savePath wins if the file exists there, else bundlePath.
static char* resolveForRead(OverlayFileSystem* ofs, const char* relativePath) {
    char* normalized = normalizePath(relativePath);
    if (isAbsolute(normalized)) return normalized;

    // Already-resolved passthrough. savePath is tested FIRST and must stay first: on the
    // PSP it is bundlePath + "saves/", so bundlePath is a literal prefix of savePath and
    // the second test matches save-resolved paths too. Both arms return the same value
    // today, which is the only reason the order is merely cosmetic rather than a bug.
    if (strncmp(normalized, ofs->savePath, strlen(ofs->savePath)) == 0) return normalized;
    if (strncmp(normalized, ofs->bundlePath, strlen(ofs->bundlePath)) == 0) return normalized;

    char* saveFull = joinPath(ofs->savePath, normalized);
    if (pathExists(saveFull)) {
        free(normalized);
        return saveFull;
    }
    free(saveFull);
    char* bundleFull = joinPath(ofs->bundlePath, normalized);
    free(normalized);
    return bundleFull;
}

// Returns a heap-allocated full path for writes. Absolute inputs pass through as-is.
// For relative inputs, it will always be the save path.
static char* resolveForWrite(OverlayFileSystem* ofs, const char* relativePath) {
    char* normalized = normalizePath(relativePath);
    if (isAbsolute(normalized)) return normalized;

    // Already-resolved passthrough. savePath is tested FIRST and must stay first: on the
    // PSP it is bundlePath + "saves/", so bundlePath is a literal prefix of savePath and
    // the second test matches save-resolved paths too. Both arms return the same value
    // today, which is the only reason the order is merely cosmetic rather than a bug.
    if (strncmp(normalized, ofs->savePath, strlen(ofs->savePath)) == 0) return normalized;
    if (strncmp(normalized, ofs->bundlePath, strlen(ofs->bundlePath)) == 0) return normalized;

    char* full = joinPath(ofs->savePath, normalized);
    free(normalized);
    return full;
}

// mkdir -p for the parent directory of fullPath. Used so writes to nested save paths work without the caller having to pre-create the directory tree.
static void ensureParentDir(const char* fullPath) {
    char buf[1024];
    size_t len = strlen(fullPath);
    if (len >= sizeof(buf)) return;
    memcpy(buf, fullPath, len + 1);
    char* lastSlash = strrchr(buf, '/');
    if (lastSlash == nullptr || lastSlash == buf) return;
    *lastSlash = '\0';
    // Walk forward, creating each component.
    for (size_t i = 1; len > i; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            overlayMkdir(buf);
            buf[i] = '/';
        }
    }
    overlayMkdir(buf);
}

// ===[ Vtable Implementations ]===

// Resolves a path in the "working_directory" convention (That is, it uses the bundle folder when resolving)
static char* overlayResolvePath(FileSystem* fs, const char* relativePath) {
    OverlayFileSystem* ofs = (OverlayFileSystem*) fs;
    char* normalized = normalizePath(relativePath);
    if (isAbsolute(normalized)) return normalized;

    // Already-resolved passthrough. savePath is tested FIRST and must stay first: on the
    // PSP it is bundlePath + "saves/", so bundlePath is a literal prefix of savePath and
    // the second test matches save-resolved paths too. Both arms return the same value
    // today, which is the only reason the order is merely cosmetic rather than a bug.
    if (strncmp(normalized, ofs->savePath, strlen(ofs->savePath)) == 0) return normalized;
    if (strncmp(normalized, ofs->bundlePath, strlen(ofs->bundlePath)) == 0) return normalized;

    char* full = joinPath(ofs->bundlePath, normalized);
    free(normalized);
    return full;
}

#ifdef PLATFORM_PSP
// ===[ RAM tempsave — plan B: don't breathe on the stick ]===
// Every console freeze is a WRITE-class stick op (remove/create/close = FAT flash
// writes) during gameplay, and DELTARUNE's scr_tempsave rewrites "filech1_9" on
// nearly every room change / battle / load (scr_saveprocess(9)). Keep that one file
// entirely in RAM: it only matters within a session (game-over -> scr_tempload);
// the real save slots (filech1_0..2) still go to the card. A stale filech1_9 from
// an older session stays readable from the card until the first in-session tempsave.
static char* g_ramTempsave = NULL; // owned NUL-terminated copy
static bool g_ramTempsavePresent = false;

static bool isRamTempsave(const char* relativePath) {
    if (relativePath == nullptr) return false;
    const char* base = strrchr(relativePath, '/');
    base = base ? base + 1 : relativePath;
    // "filech<chapter>_9" is the per-session tempsave (scr_saveprocess(9)) for ANY chapter,
    // not just ch1: the ch5 build rewrites filech5_9 on every room change too. Matching only
    // "filech1_9" left ch5's tempsave hitting the stick each room (the exact write storm this
    // RAM-plan-B exists to avoid).
    if (strncmp(base, "filech", 6) != 0) return false;
    const char* p = base + 6;
    if (*p < '0' || *p > '9') return false;   // require at least one chapter digit
    while (*p >= '0' && *p <= '9') p++;        // consume the chapter number
    return strcmp(p, "_9") == 0;              // exactly the _9 tempsave slot
}

// Save-class files (real slots + dr.ini) go through the bs_io write-behind queue
// and its resident RAM cache: a synchronous save cost ~865ms of main-thread stall
// (two files, each open+write+close on a slow stick). The tempsave never reaches
// here (isRamTempsave handles it first).
#include "psp/bg_io.h"
static bool isSaveClassPath(const char* relativePath) {
    if (relativePath == nullptr) return false;
    const char* base = strrchr(relativePath, '/');
    base = base ? base + 1 : relativePath;
    // keyconfig_N.ini is part of every scr_save too — without it each save
    // re-read it from the stick (and its ini_open evicted dr.ini from the
    // single-slot parsed-ini cache, forcing dr.ini back off the stick as well).
    // "config" also matches the file-select menu's probe of legacy config_N.ini
    // (never exists; each probe was a 13ms FAT miss until cached).
    return strncmp(base, "filech", 6) == 0 || strcmp(base, "dr.ini") == 0
        || strncmp(base, "keyconfig", 9) == 0 || strncmp(base, "config", 6) == 0;
}
#endif

#ifdef PLATFORM_PSP
// TEMP SAVE-PROFILE: file_exists costs ~17ms/call on the console (save-slot checks
// stall the save-star menu). Dumped and reset by the PERF window in psp/main.c.
unsigned int sceKernelGetSystemTimeLow(void);
unsigned int g_fexCalls, g_fexTotalUs, g_fexResolveUs;
#define FEX_T() sceKernelGetSystemTimeLow()
#else
#define FEX_T() 0u
#endif

// file_exists result cache. On the PSP every stat is a FAT directory walk (~8ms), the
// old path stat'ed TWICE (resolveForRead probed the save dir, then pathExists probed
// the result again), and the save-star menu re-checks the same few save slots over and
// over: 23 calls = ~380ms of stall. Any mutation through this filesystem bumps the
// generation, instantly staling every entry — a stale/missing entry just re-stats, so
// the cache can never return wrong results from our own writes/deletes.
#define FEX_CACHE_SLOTS 16
#define FEX_KEY_MAX 96
typedef struct { char key[FEX_KEY_MAX]; uint32_t gen; bool exists; bool used; } FexCacheEntry;
static FexCacheEntry g_fexCache[FEX_CACHE_SLOTS];
static uint32_t g_fexGen = 1;
static int g_fexCacheNext = 0;

static void fexInvalidateAll(void) { g_fexGen++; }

// Targeted invalidation for writes: nuking the whole cache on every save forced the
// save-star menu to re-stat all slots after each save (11 fresh stats, ~140ms). A
// write only changes ONE file's existence — clear just that key (normalized, so
// "./file" and "file" hit the same entry). deleteFile keeps the full nuke for safety.
static void fexInvalidatePath(const char* relativePath) {
    if (relativePath == nullptr) { fexInvalidateAll(); return; }
    char* normalized = normalizePath(relativePath);
    for (int i = 0; i < FEX_CACHE_SLOTS; i++) {
        if (g_fexCache[i].used && g_fexCache[i].gen == g_fexGen && strcmp(g_fexCache[i].key, normalized) == 0)
            g_fexCache[i].used = false;
    }
    free(normalized);
}

static bool fexCacheLookup(const char* key, bool* out) {
    if (key == nullptr || strlen(key) >= FEX_KEY_MAX) return false;
    for (int i = 0; i < FEX_CACHE_SLOTS; i++) {
        if (g_fexCache[i].used && g_fexCache[i].gen == g_fexGen && strcmp(g_fexCache[i].key, key) == 0) {
            *out = g_fexCache[i].exists;
            return true;
        }
    }
    return false;
}

static void fexCacheStore(const char* key, bool exists) {
    if (key == nullptr || strlen(key) >= FEX_KEY_MAX) return;
    FexCacheEntry* e = &g_fexCache[g_fexCacheNext];
    g_fexCacheNext = (g_fexCacheNext + 1) % FEX_CACHE_SLOTS;
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->gen = g_fexGen;
    e->exists = exists;
    e->used = true;
}

// A completed write/delete doesn't just stale the old answer — it DICTATES the new
// one (the file now exists / is gone). Storing that instead of invalidating means the
// save-star menu's re-checks after a save hit the cache instead of re-statting the
// stick (each miss is a ~14ms FAT walk; a save used to leave 10 of them behind).
static void fexNotePath(const char* relativePath, bool exists) {
    if (relativePath == nullptr) return;
    char* normalized = normalizePath(relativePath);
    fexInvalidatePath(relativePath); // drop any duplicate entry for this key first
    fexCacheStore(normalized, exists);
    free(normalized);
}

static bool overlayFileExists(FileSystem* fs, const char* relativePath) {
    OverlayFileSystem* ofs = (OverlayFileSystem*) fs;
#ifdef PLATFORM_PSP
    if (isRamTempsave(relativePath) && g_ramTempsavePresent) return true;
    // A save-class file living in the bs_io RAM cache provably exists — skip the
    // ~14ms FAT stat entirely (the first save-star open did 11 of them). The
    // negative is cacheable too: an empty slot's failed boot warm-read means the
    // card doesn't have it, and a FAT stat MISS is the most expensive kind.
    if (isSaveClassPath(relativePath)) {
        char* n = normalizePath(relativePath);
        BgIo_waitKey(n); // boot race: a pending warm answers cheaper than a FAT stat
        bool hit = BgIo_hasCached(n);
        bool missing = !hit && BgIo_knownMissing(n);
        free(n);
        if (hit) return true;
        if (missing) return false;
    }
#endif
    // Cache keys are NORMALIZED paths so writers and checkers agree on the key even
    // when one says "./file" and the other "file".
    char* normalized = normalizePath(relativePath);
    bool cached;
    if (fexCacheLookup(normalized, &cached)) {
        free(normalized);
        return cached;
    }

    unsigned int fexT0 = FEX_T();
    // Single-stat resolve (instead of resolveForRead + a second stat): save dir wins,
    // bundle dir is the fallback — the stat itself IS the existence answer.
    bool exists;
    if (isAbsolute(normalized)
        || strncmp(normalized, ofs->savePath, strlen(ofs->savePath)) == 0
        || strncmp(normalized, ofs->bundlePath, strlen(ofs->bundlePath)) == 0) {
        exists = pathExists(normalized);
    } else {
        char* saveFull = joinPath(ofs->savePath, normalized);
        exists = pathExists(saveFull);
        free(saveFull);
        if (!exists) {
            char* bundleFull = joinPath(ofs->bundlePath, normalized);
            exists = pathExists(bundleFull);
            free(bundleFull);
        }
    }
    fexCacheStore(normalized, exists);
#ifdef PLATFORM_PSP
    // Save-class negatives are the whole truth (no bundle copies) — remember the
    // miss so the NEXT probe of an empty slot / legacy config skips the FAT scan.
    if (!exists && isSaveClassPath(relativePath)) BgIo_noteMissing(normalized);
    g_fexCalls++; g_fexTotalUs += FEX_T() - fexT0;
#else
    (void) fexT0;
#endif
    free(normalized);
    return exists;
}

#ifdef PLATFORM_PSP
#include <pspiofilemgr.h> // raw sceIo file writes (stdio writes freeze the console)
// TEMP DIAG: the console freeze reproduces during save-file reading — bracket the
// stdio calls so the last trace line names the exact one (see src/psp/bs_diag.h).
void BsDiag_trace(const char* fmt, int a, int b);
#define OFS_MARK(tag) BsDiag_trace(tag, 0, 0)
#else
#define OFS_MARK(tag)
#endif

static char* overlayReadFileText(FileSystem* fs, const char* relativePath) {
#ifdef PLATFORM_PSP
    if (isRamTempsave(relativePath) && g_ramTempsavePresent) {
        size_t len = strlen(g_ramTempsave);
        char* out = (char*) safeMalloc(len + 1);
        memcpy(out, g_ramTempsave, len + 1);
        OFS_MARK("ofs ramread");
        return out;
    }
    // Save-class reads come from the resident RAM cache when possible (also the
    // read-coherency source while a write-behind for the file is still in flight).
    bool saveClass = isSaveClassPath(relativePath);
    if (saveClass) {
        char* normalized = normalizePath(relativePath);
        BgIo_waitKey(normalized); // boot race: let an in-flight warm land instead of a competing stick read
        char* cached = BgIo_lookupCachedCopy(normalized);
        // TEMP SAVETRACE: same reason as the write side -- "ofs ramhit" named no file, so a run
        // could not say which slot was served from RAM and how big it came back. A slot that
        // reads back short is the shape the reported corruption takes.
        {
            char dl[128];
            snprintf(dl, sizeof(dl), "saver %s ram=%d bytes=%d",
                     normalized != nullptr ? normalized : "?", cached != nullptr ? 1 : 0,
                     cached != nullptr ? (int) strlen(cached) : -1);
            BsDiag_trace(dl, 0, 0);
        }
        free(normalized);
        if (cached != nullptr) { OFS_MARK("ofs ramhit"); return cached; }
    }
#endif
    char* fullPath = resolveForRead((OverlayFileSystem*) fs, relativePath);
    OFS_MARK("ofs fopen");
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    if (f == nullptr) { OFS_MARK("ofs fopen-null"); return nullptr; }
    OFS_MARK("ofs fopen-ok");

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    OFS_MARK("ofs seek-ok");

    char* content = (char *)safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    OFS_MARK("ofs read-ok");
    fclose(f);
    OFS_MARK("ofs close-ok");
#ifdef PLATFORM_PSP
    // First disk read of a save-class file warms the cache: the next open is RAM-only.
    if (saveClass) {
        char* normalized = normalizePath(relativePath);
        { // the size that came off the card, which is what a truncated slot shows up as
            char dl[128];
            snprintf(dl, sizeof(dl), "saver %s disk bytes=%d",
                     normalized != nullptr ? normalized : "?", (int) bytesRead);
            BsDiag_trace(dl, 0, 0);
        }
        BgIo_noteFileContents(normalized, content);
        free(normalized);
    }
#endif
    return content;
}

static bool overlayWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    fexInvalidatePath(relativePath); // a write can create the file
#ifdef PLATFORM_PSP
    // Plan B: the tempsave never touches the stick.
    if (isRamTempsave(relativePath)) {
        size_t len = strlen(contents);
        char* copy = (char*) safeMalloc(len + 1);
        memcpy(copy, contents, len + 1);
        free(g_ramTempsave);
        g_ramTempsave = copy;
        g_ramTempsavePresent = true;
        OFS_MARK("ofs ramsave");
        return true;
    }
#endif
    char* fullPath = resolveForWrite((OverlayFileSystem*) fs, relativePath);
    ensureParentDir(fullPath);
#ifdef PLATFORM_PSP
    // Save-class writes go to the bs_io write-behind queue: main returns in
    // microseconds, the worker lands the bytes via temp-file + rename. The save
    // menu is held open by a fast-path until the queue drains ("safe mode").
    if (isSaveClassPath(relativePath)) {
        char* normalized = normalizePath(relativePath);
        bool queued = BgIo_enqueueWrite(normalized, fullPath, contents);
        // TEMP SAVETRACE: the bare "ofs wqueue" mark named no file and no size, so a run where
        // a save came back gutted could not say WHICH file was written, or whether the slot was
        // written at all. It could not: the failing run showed only reads and the RAM tempsave.
        // Name the file and the byte count, or this path stays undiagnosable.
        {
            char dl[128];
            snprintf(dl, sizeof(dl), "savew %s bytes=%d queued=%d",
                     normalized != nullptr ? normalized : "?", (int) strlen(contents), queued ? 1 : 0);
            BsDiag_trace(dl, 0, 0);
        }
        free(normalized);
        if (queued) {
            free(fullPath);
            fexNotePath(relativePath, true);
            OFS_MARK("ofs wqueue");
            return true;
        }
        // Queue full — fall through to the synchronous write below.
    }
    // The PSP console freezes inside stdio fopen("wb")/fwrite/fclose on the memory
    // stick (every crash scenario ended in a stdio file WRITE: temp-save on load,
    // manual save, the old perf-block append; stdio READS of the same files are fine,
    // and thousands of raw sceIoWrite trace lines work flawlessly). Write through the
    // raw sceIo API instead of newlib stdio.
    OFS_MARK("ofs wopen");
    // O_CREAT|O_TRUNC alone replaces the old remove-then-create: one FAT directory
    // operation instead of two per save (each is a ~10-30ms stick walk). The v24/v25
    // "no-close/no-sync" freeze experiments that motivated fresh-create were chasing
    // a symptom of the display-list overflow (fixed by dlReserve in psp_renderer.c).
    SceUID fd = sceIoOpen(fullPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    free(fullPath);
    if (fd < 0) { OFS_MARK("ofs wopen-fail"); return false; }
    OFS_MARK("ofs wopen-ok");
    size_t len = strlen(contents);
    int written = sceIoWrite(fd, contents, (unsigned) len);
    OFS_MARK("ofs wr-ok");
    sceIoClose(fd);
    OFS_MARK("ofs wclose-ok");
    bool ok = written >= 0 && (size_t) written == len;
    if (ok) fexNotePath(relativePath, true); // the file provably exists now
    return ok;
#else
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    if (f == nullptr) return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    bool ok = written == len;
    if (ok) fexNotePath(relativePath, true);
    return ok;
#endif
}

static bool overlayDeleteFile(FileSystem* fs, const char* relativePath) {
    // Targeted: only this key's answer changed. NOT stored as "false" — the same
    // relative path may still resolve to a bundle-dir copy, so the next check
    // re-stats. (The old full nuke also killed every cached save-slot answer.)
    fexInvalidatePath(relativePath);
#ifdef PLATFORM_PSP
    if (isRamTempsave(relativePath)) {
        free(g_ramTempsave);
        g_ramTempsave = NULL;
        g_ramTempsavePresent = false;
        return true; // never touch the stick for the tempsave
    }
    if (isSaveClassPath(relativePath)) {
        // A queued write finishing AFTER our remove would resurrect the file —
        // drain first (deletes are rare menu actions, the stall is acceptable).
        BgIo_waitDrain();
        char* normalized = normalizePath(relativePath);
        BgIo_dropCached(normalized);
        free(normalized);
    }
    OFS_MARK("ofs delete"); // any other delete is a write-class stick op — keep it visible
#endif
    char* fullPath = resolveForWrite((OverlayFileSystem*) fs, relativePath);
    int result = remove(fullPath);
    free(fullPath);
    return result == 0;
}

static bool overlayReadFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    char* fullPath = resolveForRead((OverlayFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    if (f == nullptr) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* data = (uint8_t *)safeMalloc((size_t) size);
    size_t bytesRead = fread(data, 1, (size_t) size, f);
    fclose(f);

    *outData = data;
    *outSize = (int32_t) bytesRead;
    return true;
}

static bool overlayWriteFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    fexInvalidatePath(relativePath);
    char* fullPath = resolveForWrite((OverlayFileSystem*) fs, relativePath);
    ensureParentDir(fullPath);
#ifdef PLATFORM_PSP
    // Raw sceIo write with a normal close; O_CREAT|O_TRUNC instead of a separate
    // remove — see overlayWriteFileText.
    SceUID fd = sceIoOpen(fullPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    free(fullPath);
    if (fd < 0) return false;
    int written = sceIoWrite(fd, data, (unsigned) size);
    sceIoClose(fd);
    bool ok = written >= 0 && written == size;
    if (ok) fexNotePath(relativePath, true);
    return ok;
#else
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    if (f == nullptr) return false;

    size_t written = fwrite(data, 1, (size_t) size, f);
    fclose(f);
    bool ok = written == (size_t) size;
    if (ok) fexNotePath(relativePath, true);
    return ok;
#endif
}

// ===[ Streaming Binary I/O ]===
// Handle wraps the FILE* plus the resolved on-disk path

typedef struct {
    FILE* fp;
    char* fullPath; // already-resolved absolute path used at open time
} OverlayBinaryHandle;

static OverlayBinaryHandle* overlayBinaryHandleNew(FILE* fp, char* fullPathTaken) {
    OverlayBinaryHandle* h = (OverlayBinaryHandle *)safeMalloc(sizeof(OverlayBinaryHandle));
    h->fp = fp;
    h->fullPath = fullPathTaken; // takes ownership
    return h;
}

static void* overlayBinaryOpen(FileSystem* fs, const char* relativePath, int32_t mode) {
    OverlayFileSystem* ofs = (OverlayFileSystem*) fs;
    if (mode != GML_FILE_BIN_READ) fexInvalidatePath(relativePath); // write modes can create the file
    if (mode == GML_FILE_BIN_READ) {
        char* path = resolveForRead(ofs, relativePath);
        FILE* f = fopen(path, "rb");
        if (f == nullptr) { free(path); return nullptr; }
        return overlayBinaryHandleNew(f, path);
    }
    if (mode == GML_FILE_BIN_WRITE) {
        char* path = resolveForWrite(ofs, relativePath);
        ensureParentDir(path);
        FILE* f = fopen(path, "wb");
        if (f == nullptr) { free(path); return nullptr; }
        return overlayBinaryHandleNew(f, path);
    }
    // GML_FILE_BIN_READWRITE: preserve existing contents
    char* readPath = resolveForRead(ofs, relativePath);
    FILE* f = fopen(readPath, "r+b");
    if (f != nullptr) return overlayBinaryHandleNew(f, readPath);
    free(readPath);
    char* writePath = resolveForWrite(ofs, relativePath);
    ensureParentDir(writePath);
    f = fopen(writePath, "w+b");
    if (f == nullptr) { free(writePath); return nullptr; }
    return overlayBinaryHandleNew(f, writePath);
}

static void overlayBinaryClose(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return;
    OverlayBinaryHandle* h = (OverlayBinaryHandle*) handle;
    if (h->fp != nullptr) fclose(h->fp);
    free(h->fullPath);
    free(h);
}

static int32_t overlayBinaryRead(MAYBE_UNUSED FileSystem* fs, void* handle, void* dst, int32_t n) {
    if (handle == nullptr || 0 >= n) return 0;
    return (int32_t) fread(dst, 1, (size_t) n, ((OverlayBinaryHandle*) handle)->fp);
}

static int32_t overlayBinaryWrite(MAYBE_UNUSED FileSystem* fs, void* handle, const void* src, int32_t n) {
    if (handle == nullptr || 0 >= n) return 0;
    return (int32_t) fwrite(src, 1, (size_t) n, ((OverlayBinaryHandle*) handle)->fp);
}

static int32_t overlayBinaryTell(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return 0;
    return (int32_t) ftell(((OverlayBinaryHandle*) handle)->fp);
}

static bool overlayBinarySeek(MAYBE_UNUSED FileSystem* fs, void* handle, int32_t pos) {
    if (handle == nullptr) return false;
    return fseek(((OverlayBinaryHandle*) handle)->fp, pos, SEEK_SET) == 0;
}

static int32_t overlayBinarySize(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return 0;
    FILE* f = ((OverlayBinaryHandle*) handle)->fp;
    long saved = ftell(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, saved, SEEK_SET);
    return (int32_t) size;
}

static void overlayBinaryRewrite(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return;
    OverlayBinaryHandle* h = (OverlayBinaryHandle*) handle;
    if (h->fp != nullptr) fclose(h->fp);
    // "wb+" truncates the existing file (or creates it if it vanished since open) and gives back read+write
    h->fp = fopen(h->fullPath, "wb+");
}

static bool overlayDirectoryExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = resolveForRead((OverlayFileSystem*) fs, relativePath);
    struct stat st;
    bool exists = (stat(fullPath, &st) == 0 && S_ISDIR(st.st_mode));
    free(fullPath);
    return exists;
}

static bool overlayCreateDirectory(FileSystem* fs, const char* relativePath) {
    char* fullPath = resolveForWrite((OverlayFileSystem*) fs, relativePath);
    int result = overlayMkdir(fullPath);
    free(fullPath);
    return result == 0;
}

static bool overlayDeleteDirectory(FileSystem* fs, const char* relativePath) {
    char* fullPath = resolveForWrite((OverlayFileSystem*) fs, relativePath);
    int result = overlayRmdir(fullPath);
    free(fullPath);
    return result == 0;
}

// ===[ Directory Enumeration ]===

// Appends an entry, deduping by name.
static void dirListPush(FileSystemDirEntry** list, const char* name, bool isDirectory) {
    repeat(arrlen(*list), i) {
        if (strcmp((*list)[i].name, name) == 0) return;
    }
    FileSystemDirEntry entry = {0};
    entry.name = safeStrdup(name);
    entry.isDirectory = isDirectory;
    arrput(*list, entry);
}

// Enumerates a single on-disk directory, appending its entries to the list. Missing directories are silently skipped.
static void listSingleDir(FileSystemDirEntry** list, const char* fullDir) {
#ifdef _WIN32
    // FindFirstFileA wants a "<dir>/*" search pattern.
    size_t dirLen = strlen(fullDir);
    char* search = (char *)safeMalloc(dirLen + 3);
    memcpy(search, fullDir, dirLen);
    search[dirLen] = '/';
    search[dirLen + 1] = '*';
    search[dirLen + 2] = '\0';
    WIN32_FIND_DATAA findData;
    HANDLE h = FindFirstFileA(search, &findData);
    free(search);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char* name = findData.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        bool isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        dirListPush(list, name, isDir);
    } while (FindNextFileA(h, &findData));
    FindClose(h);
#else
    DIR* dir = opendir(fullDir);
    if (dir == nullptr) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        // d_type may be DT_UNKNOWN on some filesystems, so stat to be sure.
        bool isDir = false;
        size_t dirLen = strlen(fullDir);
        size_t nameLen = strlen(ent->d_name);
        char* full = (char *)safeMalloc(dirLen + nameLen + 2);
        memcpy(full, fullDir, dirLen);
        full[dirLen] = '/';
        memcpy(full + dirLen + 1, ent->d_name, nameLen + 1);
        struct stat st;
        if (stat(full, &st) == 0) isDir = S_ISDIR(st.st_mode);
        free(full);
        dirListPush(list, ent->d_name, isDir);
    }
    closedir(dir);
#endif
}

static FileSystemDirEntry* overlayListDirectory(FileSystem* fs, const char* relativeDirPath) {
    OverlayFileSystem* ofs = (OverlayFileSystem*) fs;
    char* normalized = normalizePath(relativeDirPath);
    FileSystemDirEntry* list = nullptr;
    if (isAbsolute(normalized)) {
        listSingleDir(&list, normalized);
    } else {
        // Save area first so it wins on dedupe, matching the read overlay (save shadows bundle).
        char* saveFull = joinPath(ofs->savePath, normalized);
        listSingleDir(&list, saveFull);
        free(saveFull);
        char* bundleFull = joinPath(ofs->bundlePath, normalized);
        listSingleDir(&list, bundleFull);
        free(bundleFull);
    }
    free(normalized);
    return list;
}

// ===[ Vtable ]===

static FileSystemVtable overlayFileSystemVtable;

// ===[ Lifecycle ]===

// Returns a heap-allocated copy of `path` guaranteed to end with '/'. Empty input becomes "./".
static char* withTrailingSlash(const char* path) {
    if (path == nullptr || path[0] == '\0') return safeStrdup("./");
    size_t len = strlen(path);
    char last = path[len - 1];
    if (last == '/' || last == '\\') {
        char* out = safeStrdup(path);
        if (last == '\\') out[len - 1] = '/';
        return out;
    }
    char* out = (char *)safeMalloc(len + 2);
    memcpy(out, path, len);
    out[len] = '/';
    out[len + 1] = '\0';
    return out;
}

OverlayFileSystem* OverlayFileSystem_create(const char* bundlePath, const char* savePath) {
    OverlayFileSystem* fs = (OverlayFileSystem *)safeCalloc(1, sizeof(OverlayFileSystem));
    fs->base.vtable = &overlayFileSystemVtable;
    overlayFileSystemVtable.resolvePath = overlayResolvePath;
    overlayFileSystemVtable.fileExists = overlayFileExists;
    overlayFileSystemVtable.readFileText = overlayReadFileText;
    overlayFileSystemVtable.writeFileText = overlayWriteFileText;
    overlayFileSystemVtable.deleteFile = overlayDeleteFile;
    overlayFileSystemVtable.readFileBinary = overlayReadFileBinary;
    overlayFileSystemVtable.writeFileBinary = overlayWriteFileBinary;
    overlayFileSystemVtable.binaryOpen = overlayBinaryOpen;
    overlayFileSystemVtable.binaryClose = overlayBinaryClose;
    overlayFileSystemVtable.binaryRead = overlayBinaryRead;
    overlayFileSystemVtable.binaryWrite = overlayBinaryWrite;
    overlayFileSystemVtable.binaryTell = overlayBinaryTell;
    overlayFileSystemVtable.binarySeek = overlayBinarySeek;
    overlayFileSystemVtable.binarySize = overlayBinarySize;
    overlayFileSystemVtable.binaryRewrite = overlayBinaryRewrite;
    overlayFileSystemVtable.directoryExists = overlayDirectoryExists;
    overlayFileSystemVtable.createDirectory = overlayCreateDirectory;
    overlayFileSystemVtable.deleteDirectory = overlayDeleteDirectory;
    overlayFileSystemVtable.listDirectory = overlayListDirectory;
    fs->bundlePath = withTrailingSlash(bundlePath);
    fs->savePath = withTrailingSlash(savePath);
    return fs;
}

void OverlayFileSystem_destroy(OverlayFileSystem* fs) {
    if (fs == nullptr) return;
    free(fs->bundlePath);
    free(fs->savePath);
    free(fs);
}

#ifdef PLATFORM_PSP
void OverlayFileSystem_warmSaveCache(OverlayFileSystem* ofs, bool legacyFallback) {
    // Save sets for every shipped chapter build (one EBOOT serves them all); a
    // missing file is a cheap failed warm-read that seeds the negative cache.
    // filechN_9 (tempsave) is RAM-only. The file-select menu also probes the
    // second slot triple (filech1_3..5), so warm those too.
    static const char* kSaveFiles[] = {
        "filech1_0", "filech1_1", "filech1_2", "filech1_3", "filech1_4", "filech1_5",
        "filech5_0", "filech5_1", "filech5_2",
        "dr.ini",
        "keyconfig_0.ini", "keyconfig_1.ini", "keyconfig_2.ini",
    };
    for (size_t i = 0; i < sizeof(kSaveFiles) / sizeof(kSaveFiles[0]); i++) {
        char* fullPath = joinPath(ofs->savePath, kSaveFiles[i]);
        // The bundle dir is the fallback, and it is load-bearing rather than tidy: a warm
        // miss becomes a missing-note that overlayFileExists returns as final, BEFORE its
        // own bundle fallback ever runs. Before saves/ existed the two dirs were the same
        // and probing one was the whole truth; now a save left behind by an interrupted
        // migration would be reported as an EMPTY SLOT with its bytes still on the card.
        // Off once migration is confirmed complete — see the header.
        char* legacyPath = legacyFallback ? joinPath(ofs->bundlePath, kSaveFiles[i]) : nullptr;
        bool distinct = legacyPath != nullptr && strcmp(fullPath, legacyPath) != 0;
        BgIo_warmFile(kSaveFiles[i], fullPath, distinct ? legacyPath : nullptr);
        free(legacyPath);
        free(fullPath);
    }
}
#endif
