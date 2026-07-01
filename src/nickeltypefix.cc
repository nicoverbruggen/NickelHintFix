// NickelTypeFix — fixes several text-rendering defects in Kobo's Qt 5.2 WebKit/iType reader
// stack. Each fix is INDEPENDENT and FAIL-SAFE: it engages only if its seam is present on the
// running firmware, and a mismatch on one leaves the others working (nothing is written to disk;
// a boot without the mod is stock). Firmware 4.x only — inert on 5.x (NickelHook won't load).
//
//   1. Glyph "wobble"       — hook FT_Load_Glyph (libkobo), load unhinted   [ntf_no_hinting]
//   2. Vertical (tategaki)  — CSS override on the live page (libnickel)      [ntf_vertfix]
//   3. Justify: koboSpan     — in-memory patch, QTextEngine::justify (libQtGui)   [ntf_justify_kospan]
//   4. Justify: punctuation  — in-memory patch, isInterIdeographExpansionTarget (libQtWebKit) [ntf_justify_punct]
//
// Cause + fix for each is documented in docs/how-it-works.md. Fixes 1–2 use NickelHook PLT hooks;
// fixes 3–4 patch stripped device libs in memory (locate lib -> position-independent pattern-scan
// -> mprotect + write + flush icache). On install this mod also removes the superseded standalone
// mods (NickelHintFix, NickelJustifyFix) so they don't co-load.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE          // dl_iterate_phdr / ElfW (guard: gnu++ dialect may predefine it)
#endif
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <link.h>

#include <QString>
#include <QUrl>

#include <NickelHook.h>

#include "config.h"
#include "util.h"

// ================= shared config =================
static bool ntf_enabled() { return ntf_global_config_bool("ntf_enabled", true); }
static bool ntf_log()     { return ntf_global_config_bool("ntf_log", true); }
#define NTF_DBG(...) do { if (ntf_log()) NTF_LOG(__VA_ARGS__); } while (0)

// Built-in default config — written to <config-dir>/config by config.c when it's missing (no
// shipped 'default' file). Kept next to the keys so they stay in sync.
extern "C" const char *const ntf_default_config = R"CFG(#
# NickelTypeFix configuration
#
# Fixes several text-rendering defects in Kobo's reader (firmware 4.x). Each fix is independent:
# it engages only if its seam exists on your firmware, and sits out safely otherwise. Changes
# take effect on reboot. Nothing is written to any device library on disk — a boot without the
# mod is stock.
#

ntf_enabled:1

# Fix 1 - glyph "wobble": load glyphs unhinted so iType stops grid-fitting them to inconsistent
# pixel heights. 0 = stock rendering.
ntf_no_hinting:1
# Comma-separated font families to leave natively hinted (exempt from the above),
# e.g. Georgia, Kobo Nickel
ntf_hinting_allowlist:

# Fix 2 - vertical (tategaki) text: keep vertical CJK books on WebKit's simple path so they
# render correctly with optimizeLegibility on.
ntf_vertfix:1

# Fix 3 - justified text at koboSpan (sentence) boundaries in kepubs (the main justification fix).
ntf_justify_kospan:1
# Fix 4 - justification around punctuation (em/en dashes, ellipses, curly quotes).
ntf_justify_punct:1

# Verbose logging to nickel-type-fix.log (per fix: which lib, match count, bytes). 0 to quieten.
ntf_log:1
)CFG";

// ================= FIX 1: hinting "wobble" (libkobo / FT_Load_Glyph) =================
// Minimal FreeType shim — only what we touch (face->family_name for the allow-list).
typedef int FT_Error; typedef signed int FT_Int; typedef signed int FT_Int32; typedef unsigned int FT_UInt;
typedef unsigned short FT_UShort; typedef short FT_Short; typedef long FT_Long; typedef long FT_Pos;
typedef struct FT_BBox_ { FT_Pos xMin, yMin, xMax, yMax; } FT_BBox;
typedef struct FT_Generic_ { void *data, *finalizer; } FT_Generic;
typedef struct FT_GlyphSlotRec_ *FT_GlyphSlot;
typedef struct FT_FaceRec_ *FT_Face;
typedef struct FT_FaceRec_ {
    FT_Long num_faces, face_index, face_flags, style_flags, num_glyphs;
    char *family_name, *style_name;
    FT_Int num_fixed_sizes; void *available_sizes; FT_Int num_charmaps; void *charmaps;
    FT_Generic generic; FT_BBox bbox;
    FT_UShort units_per_EM; FT_Short ascender, descender, height, max_advance_width, max_advance_height,
    underline_position, underline_thickness; FT_GlyphSlot glyph;
} FT_FaceRec;

static const FT_Int32 NTF_FT_LOAD_NO_HINTING = 0x2;
static const char *const NTF_LIBKOBO = "/usr/local/Kobo/platforms/libkobo.so";
static FT_Error (*real_FT_Load_Glyph)(FT_Face, FT_UInt, FT_Int32) = nullptr;

// Hinting-scoped safety: if the FT path misbehaves, only the hinting fix passes through — the
// vertical + justify fixes are unaffected. The disabled-by-safety marker persists across boots.
static bool ntf_hint_disabled = false;
static bool ntf_hint_log_dumped = false;

static bool ntf_no_hinting() { return ntf_global_config_bool("ntf_no_hinting", true); }

static bool ntf_font_hinting_allowed(FT_Face face) {
    const char *list = ntf_global_config_get("ntf_hinting_allowlist");
    if (!list || !*list) return false;               // common path never touches the FT shim
    const char *family = (face && face->family_name) ? face->family_name : NULL;
    if (!family || !*family) return false;
    size_t flen = strlen(family);
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - start) == flen && !strncasecmp(family, start, flen)) return true;
    }
    return false;
}

static void ntf_hint_write_marker(const char *path, const char *msg) {
    mkdir(NTF_CONFIG_DIR, 0755);
    FILE *f = fopen(path, "w"); if (!f) return;
    if (msg) fprintf(f, "%s\n", msg);
    fclose(f);
}
static void ntf_hint_disable_for_safety(const char *reason) {
    if (ntf_hint_disabled) return;
    ntf_hint_disabled = true;
    NTF_LOG("SAFETY: disabling the hinting fix for this boot: %s", reason ? reason : "unknown");
    ntf_hint_write_marker(NTF_CONFIG_DIR "/disabled-by-safety", reason);
    if (!ntf_hint_log_dumped) { ntf_hint_log_dumped = true; nh_dump_log(); }
}
static bool ntf_hint_marker_present() {
    static int present = -1;
    if (present == -1) present = (access(NTF_CONFIG_DIR "/disabled-by-safety", F_OK) == 0) ? 1 : 0;
    return present == 1;
}

// ================= FIX 2: vertical (tategaki) text (libnickel) =================
static int  (*ntf_writingDirectionFromString)(const QString &) = nullptr;
static void *(*ntf_cwv_settings)(void *cwv) = nullptr;
static void (*ntf_setUserStyleSheetUrl)(void *settings, const QUrl &url) = nullptr;
static void (*real_cwv_setWritingDirection)(void *self, int dir) = nullptr;
static void *(*real_kepubReaderCtor)(void *self, void *pluginState, void *widget) = nullptr;

// `*{text-rendering:auto !important}` as a base64 data: URL.
static const char *const NTF_VERT_CSS_URL =
    "data:text/css;charset=utf-8;base64,Knt0ZXh0LXJlbmRlcmluZzphdXRvICFpbXBvcnRhbnR9";
static int  ntf_wd_vrl = -1, ntf_wd_vlr = -1;
static bool ntf_vertfix_ready = false;
static int  ntf_us_applied = -1;

static bool ntf_vertfix() { return ntf_global_config_bool("ntf_vertfix", true); }

static void ntf_apply_vertical_css(void *cwv, bool vertical) {
    if (!ntf_cwv_settings || !ntf_setUserStyleSheetUrl) return;
    void *settings = ntf_cwv_settings(cwv);
    if (!settings) return;
    QUrl url;  // empty clears the user stylesheet
    if (vertical) url = QUrl(QString::fromLatin1(NTF_VERT_CSS_URL));
    ntf_setUserStyleSheetUrl(settings, url);
}

// ================= FIX 3+4: justification (in-memory byte patches) =================
// TIMING: these edits run from ntf_init, which NickelHook calls from its library __constructor
// as Nickel dlopen()s this plugin at startup — long before any book is opened. The patched
// functions (QTextEngine::justify, isInterIdeographExpansionTarget) only execute during page
// layout of an opened book, so no thread is mid-execution in them when we write. That is what
// makes the non-atomic 2-byte writes safe; keep the patch phase in init if init ordering changes.
// koboSpan fix — libQtGui, QTextEngine::justify, two sites (both required, both-or-nothing).
static const unsigned char KOS_A_ANCHOR[] = { 0x15,0xF8,0x01,0x3C, 0xD8,0x06, 0x40,0xF1,0x9E,0x80, 0x04,0xE0 };
static const unsigned char KOS_A_ORIG[]   = { 0x40,0xF1,0x9E,0x80 };   // bpl.w -> b.w (skip trim loop)
static const unsigned char KOS_A_REPL[]   = { 0x00,0xF0,0x9E,0xB8 };
static const unsigned char KOS_B_ANCHOR[] = { 0x2C,0x46, 0x51,0xE7, 0x63,0x1E, 0x3B,0x61, 0xDC,0xD0 };
static const unsigned char KOS_B_ORIG[]   = { 0x63,0x1E };             // subs r3,r4,#1 -> movs r3,r4
static const unsigned char KOS_B_REPL[]   = { 0x23,0x00 };
// punctuation fix — libQtWebKit, isInterIdeographExpansionTarget, one site (anchor+0x18).
static const unsigned char PUN_ANCHOR[] = {
    0xa0,0xf5,0x00,0x52, 0xa2,0xf1,0x10,0x03, 0x01,0x2b, 0x8c,0xbf,
    0x01,0x23, 0x00,0x23, 0x6f,0x2a, 0x88,0xbf, 0x00,0x23, 0x0b,0xb1,
};
static const unsigned char PUN_ORIG[] = { 0x18,0x46 };   // mov r0,r3 -> movs r0,#0
static const unsigned char PUN_REPL[] = { 0x00,0x20 };

struct ntf_patch_t {
    const char *label; const char *incl, *excl;
    const unsigned char *anchor; int anchor_len; int off;
    const unsigned char *orig, *repl; int plen;
};
#define NTF_MAXP 2
struct ntf_fix_t { const char *name, *cfg_key; bool cfg_default; struct ntf_patch_t patch[NTF_MAXP]; int n; };

static const struct ntf_fix_t NTF_JUSTIFY_FIXES[] = {
    { "koboSpan (QTextEngine::justify)", "ntf_justify_kospan", true, {
        { "justify:skip-trim", "Gui", NULL,    KOS_A_ANCHOR, (int)sizeof(KOS_A_ANCHOR), 6, KOS_A_ORIG, KOS_A_REPL, (int)sizeof(KOS_A_ORIG) },
        { "justify:range-len", "Gui", NULL,    KOS_B_ANCHOR, (int)sizeof(KOS_B_ANCHOR), 4, KOS_B_ORIG, KOS_B_REPL, (int)sizeof(KOS_B_ORIG) },
    }, 2 },
    { "punctuation (isInterIdeographExpansionTarget)", "ntf_justify_punct", true, {
        { "expansion-target", "WebKit", "Widgets", PUN_ANCHOR, (int)sizeof(PUN_ANCHOR), (int)sizeof(PUN_ANCHOR), PUN_ORIG, PUN_REPL, (int)sizeof(PUN_ORIG) },
        { 0 },
    }, 1 },
};

static const unsigned char *ntf_scan(const unsigned char *hay, size_t haylen,
                                     const unsigned char *needle, size_t nlen, int *count) {
    const unsigned char *first = NULL; int c = 0;
    if (haylen >= nlen)
        for (size_t i = 0; i + nlen <= haylen; i++)
            if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0) { if (!first) first = hay + i; c++; }
    *count = c; return first;
}
struct ntf_find { const char *incl, *excl; const unsigned char *needle; int nlen; int total; const unsigned char *match; };
static int ntf_find_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; struct ntf_find *f = (struct ntf_find *)data;
    const char *name = info->dlpi_name;
    if (!name || !*name) return 0;
    if (!strstr(name, f->incl)) return 0;
    if (f->excl && strstr(name, f->excl)) return 0;   // e.g. exclude the small WebKitWidgets wrapper
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) continue;
        const unsigned char *seg = (const unsigned char *)(info->dlpi_addr + ph->p_vaddr);
        int c = 0;
        const unsigned char *m = ntf_scan(seg, (size_t)ph->p_memsz, f->needle, (size_t)f->nlen, &c);
        if (c > 0) { if (!f->match) f->match = m; f->total += c; }
    }
    return 0;
}
static void ntf_forceload(void) {
    static const char *cands[] = {
        "libQt5WebKit.so.5", "libQtWebKit.so.4", "libQt5Gui.so.5", "libQtGui.so.4",
        "/usr/local/Qt-5.2.1-arm/lib/libQt5WebKit.so.5", "/usr/local/Qt-5.2.1-arm/lib/libQt5Gui.so.5",
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        void *h = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
        if (!h) h = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL);
        (void)h;
    }
}
static bool ntf_write(const unsigned char *site, const unsigned char *repl, int len) {
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 4096;
    uintptr_t addr = (uintptr_t)site, page = addr & ~(uintptr_t)(pg - 1);
    size_t span = (size_t)((addr + (unsigned)len) - page);
    if (mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        NTF_LOG("mprotect(RWX) failed at %p: %s", (void *)page, strerror(errno)); return false;
    }
    for (int i = 0; i < len; i++) ((unsigned char *)site)[i] = repl[i];
    __builtin___clear_cache((char *)site, (char *)site + len);
    if (mprotect((void *)page, span, PROT_READ | PROT_EXEC) != 0)
        NTF_LOG("warning: mprotect(R-X) restore failed at %p: %s", (void *)page, strerror(errno));
    return true;
}
// Locate + verify every edit in a fix; write them only if all located and verified (both-or-nothing).
static void ntf_apply_justify_fix(const struct ntf_fix_t *fx) {
    if (!ntf_global_config_bool(fx->cfg_key, fx->cfg_default)) { NTF_LOG("justify fix '%s' off (%s:0)", fx->name, fx->cfg_key); return; }
    const unsigned char *sites[NTF_MAXP]; bool already[NTF_MAXP];
    for (int i = 0; i < fx->n; i++) {
        const struct ntf_patch_t *p = &fx->patch[i];
        struct ntf_find f = { p->incl, p->excl, p->anchor, p->anchor_len, 0, NULL };
        dl_iterate_phdr(ntf_find_cb, &f);
        NTF_DBG("  [%s] %s: matches=%d", fx->name, p->label, f.total);
        if (f.total == 0) { NTF_LOG("justify fix '%s' skipped: '%s' pattern not found", fx->name, p->label); return; }
        if (f.total > 1)  { NTF_LOG("justify fix '%s' skipped: '%s' ambiguous (%d)", fx->name, p->label, f.total); return; }
        const unsigned char *site = f.match + p->off; already[i] = false;
        if (memcmp(site, p->repl, (size_t)p->plen) == 0) { NTF_DBG("  [%s] %s already patched", fx->name, p->label); already[i] = true; }
        else if (memcmp(site, p->orig, (size_t)p->plen) != 0) { NTF_LOG("justify fix '%s' skipped: '%s' unexpected bytes at %p", fx->name, p->label, (const void *)site); return; }
        sites[i] = site;
    }
    int wrote = 0;
    for (int i = 0; i < fx->n; i++) if (!already[i] && ntf_write(sites[i], fx->patch[i].repl, fx->patch[i].plen)) wrote++;
    NTF_LOG("justify fix '%s' APPLIED (%d edit(s))", fx->name, wrote);
}

// ================= startup: remove the superseded standalone mods =================
static void ntf_rmtree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[1024]; snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat st;
            if (!lstat(child, &st) && S_ISDIR(st.st_mode)) ntf_rmtree(child); else unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
}
static void ntf_remove_superseded(void) {
    static const char *old_so[] = {
        "/usr/local/Kobo/imageformats/libnickelhintfix.so",
        "/usr/local/Kobo/imageformats/libnickeljustifyfix.so",
    };
    for (size_t i = 0; i < sizeof(old_so) / sizeof(old_so[0]); i++)
        if (access(old_so[i], F_OK) == 0 && unlink(old_so[i]) == 0)
            NTF_LOG("removed superseded plugin %s", old_so[i]);
    static const char *old_dir[] = {
        "/mnt/onboard/.adds/nickelhintfix", "/mnt/onboard/.adds/nickeljustifyfix",
    };
    for (size_t i = 0; i < sizeof(old_dir) / sizeof(old_dir[0]); i++)
        if (access(old_dir[i], F_OK) == 0) { ntf_rmtree(old_dir[i]); NTF_LOG("removed superseded config dir %s", old_dir[i]); }
}

// ================= init =================
static int ntf_init() {
    ntf_global_config_get("");                      // prime config while single-threaded
    ntf_remove_superseded();                        // stop the old standalone mods co-loading
    NTF_LOG("startup: enabled=%d no_hinting=%d vertfix=%d justify_kospan=%d justify_punct=%d log=%d",
        ntf_enabled(), ntf_no_hinting(), ntf_vertfix(),
        ntf_global_config_bool("ntf_justify_kospan", true), ntf_global_config_bool("ntf_justify_punct", true), ntf_log());
    if (!ntf_enabled()) { NTF_LOG("disabled by config; nothing applied"); return 0; }

    // FIX 2 (vertical): learn the vertical-writing-mode enum values from Nickel itself.
    NTF_LOG("startup: vertical syms cwvSetDir=%p cwvSettings=%p setUserCss=%p kepubCtor=%p wdFromString=%p",
        (void *)real_cwv_setWritingDirection, (void *)ntf_cwv_settings, (void *)ntf_setUserStyleSheetUrl,
        (void *)real_kepubReaderCtor, (void *)ntf_writingDirectionFromString);
    if (ntf_writingDirectionFromString) {
        ntf_wd_vrl = ntf_writingDirectionFromString(QStringLiteral("vertical-rl"));
        ntf_wd_vlr = ntf_writingDirectionFromString(QStringLiteral("vertical-lr"));
        ntf_vertfix_ready = true;
        NTF_LOG("startup: vertical-rl=%d vertical-lr=%d", ntf_wd_vrl, ntf_wd_vlr);
    } else {
        NTF_LOG("startup: writingDirectionFromString unresolved; vertical fix inert");
    }
    if (ntf_hint_marker_present()) NTF_LOG("startup: hinting disabled-by-safety marker present; hinting passes through");

    // FIX 3+4 (justify): pattern-scan + patch the loaded libs in memory.
    ntf_forceload();
    for (size_t i = 0; i < sizeof(NTF_JUSTIFY_FIXES) / sizeof(NTF_JUSTIFY_FIXES[0]); i++)
        ntf_apply_justify_fix(&NTF_JUSTIFY_FIXES[i]);
    return 0;
}

// ================= uninstall / wiring =================
static bool ntf_del(const char *p) { return (access(p, F_OK) && errno == ENOENT) ? true : nh_delete_file(p); }
static bool ntf_uninstall() {
    NTF_LOG("uninstall: removing NickelTypeFix files (in-memory patches revert on next boot)");
    bool ok = true;
    ok = ntf_del(NTF_CONFIG_DIR "/doc") && ok;
    ok = ntf_del(NTF_CONFIG_DIR "/config") && ok;
    ok = ntf_del(NTF_CONFIG_DIR "/nickel-type-fix.log") && ok;
    ok = ntf_del(NTF_CONFIG_DIR "/disabled-by-safety") && ok;
    ok = ntf_del(NTF_CONFIG_DIR "/uninstall") && ok;
    if (access(NTF_CONFIG_DIR, F_OK) == 0) ok = nh_delete_dir(NTF_CONFIG_DIR) && ok;
    return ok;
}

static struct nh_info NickelTypeFixInfo = {
    .name            = "NickelTypeFix",
    .desc            = "Fix Kobo reader text rendering: hinting wobble, vertical text, and justification.",
    .uninstall_flag  = NTF_CONFIG_DIR "/uninstall-now",
    .uninstall_xflag = NTF_CONFIG_DIR "/uninstall",
    .failsafe_delay  = 3,
};
static struct nh_hook NickelTypeFixHooks[] = {
    // FIX 1 — now OPTIONAL so a missing FT symbol only sits out hinting (independence).
    { .sym = "FT_Load_Glyph", .sym_new = "_ntf_FT_Load_Glyph", .lib = NTF_LIBKOBO,
      .out = nh_symoutptr(real_FT_Load_Glyph), .desc = "load glyphs unhinted", .optional = true },
    // FIX 2 — optional.
    { .sym = "_ZN15KepubBookReaderC1EP11PluginStateP7QWidget", .sym_new = "_ntf_kepubReaderCtor",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_kepubReaderCtor), .desc = "reset per-book state", .optional = true },
    { .sym = "_ZN13CustomWebView19setWritingDirectionE16WritingDirection", .sym_new = "_ntf_cwv_setWritingDirection",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(real_cwv_setWritingDirection), .desc = "inject text-rendering:auto for vertical books", .optional = true },
    {0},
};
static struct nh_dlsym NickelTypeFixDlsym[] = {
    { .name = "_Z26writingDirectionFromStringRK7QString", .out = nh_symoutptr(ntf_writingDirectionFromString), .desc = "derive vertical enum ints", .optional = true },
    { .name = "_ZNK13CustomWebView8settingsEv", .out = nh_symoutptr(ntf_cwv_settings), .desc = "reach the page's QWebSettings", .optional = true },
    { .name = "_ZN12QWebSettings20setUserStyleSheetUrlERK4QUrl", .out = nh_symoutptr(ntf_setUserStyleSheetUrl), .desc = "set/clear the user stylesheet", .optional = true },
    {0},
};

NickelHook(
    .init      = &ntf_init,
    .info      = &NickelTypeFixInfo,
    .hook      = NickelTypeFixHooks,
    .dlsym     = NickelTypeFixDlsym,
    .uninstall = &ntf_uninstall,
)

// ================= hook bodies =================
// FIX 1 — hinting. Independent: if disabled-by-safety, only hinting passes through.
extern "C" __attribute__((visibility("default")))
FT_Error _ntf_FT_Load_Glyph(FT_Face face, FT_UInt glyph_index, FT_Int32 load_flags) {
    if (!real_FT_Load_Glyph) { ntf_hint_disable_for_safety("real FT_Load_Glyph was NULL"); return 1; }
    if (ntf_hint_disabled || ntf_hint_marker_present())
        return real_FT_Load_Glyph(face, glyph_index, load_flags);
    // Orthogonal to iType's CSM stem-weighting (Font Weight) — that's set before the load.
    FT_Int32 eff = load_flags;
    if (ntf_enabled() && ntf_no_hinting() && !ntf_font_hinting_allowed(face))
        eff |= NTF_FT_LOAD_NO_HINTING;
    return real_FT_Load_Glyph(face, glyph_index, eff);
}

// FIX 2 — vertical. Per-book reset, then push/clear the user stylesheet once vertical is known.
// Independent of the hinting safety state.
extern "C" __attribute__((visibility("default")))
void *_ntf_kepubReaderCtor(void *self, void *pluginState, void *widget) {
    ntf_us_applied = -1;
    if (real_kepubReaderCtor) return real_kepubReaderCtor(self, pluginState, widget);
    return self;
}
extern "C" __attribute__((visibility("default")))
void _ntf_cwv_setWritingDirection(void *self, int dir) {
    bool vert = ntf_vertfix_ready && (dir == ntf_wd_vrl || dir == ntf_wd_vlr);
    int want = vert ? 1 : 0;
    if (ntf_enabled() && ntf_vertfix() && ntf_vertfix_ready && want != ntf_us_applied) {
        ntf_apply_vertical_css(self, vert);
        ntf_us_applied = want;
    }
    if (real_cwv_setWritingDirection) real_cwv_setWritingDirection(self, dir);
}
