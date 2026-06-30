#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QString>
#include <QUrl>

#include <NickelHook.h>

#include "config.h"
#include "util.h"

// Minimal FreeType type shims — only what we touch. We read face->family_name,
// so FT_FaceRec's layout up to that field must match the device's FreeType; the
// later fields are kept for fidelity. FT_BBox/FT_Generic/FT_GlyphSlot exist only
// because FT_FaceRec embeds/points to them.
typedef int FT_Error;
typedef signed int FT_Int;
typedef signed int FT_Int32;
typedef unsigned int FT_UInt;
typedef unsigned short FT_UShort;
typedef short FT_Short;
typedef long FT_Long;
typedef long FT_Pos;

typedef struct FT_BBox_ {
    FT_Pos xMin;
    FT_Pos yMin;
    FT_Pos xMax;
    FT_Pos yMax;
} FT_BBox;

typedef struct FT_Generic_ {
    void *data;
    void *finalizer;
} FT_Generic;

typedef struct FT_GlyphSlotRec_ *FT_GlyphSlot;
typedef struct FT_FaceRec_ *FT_Face;

typedef struct FT_FaceRec_ {
    FT_Long num_faces;
    FT_Long face_index;
    FT_Long face_flags;
    FT_Long style_flags;
    FT_Long num_glyphs;
    char *family_name;
    char *style_name;
    FT_Int num_fixed_sizes;
    void *available_sizes;
    FT_Int num_charmaps;
    void *charmaps;
    FT_Generic generic;
    FT_BBox bbox;
    FT_UShort units_per_EM;
    FT_Short ascender;
    FT_Short descender;
    FT_Short height;
    FT_Short max_advance_width;
    FT_Short max_advance_height;
    FT_Short underline_position;
    FT_Short underline_thickness;
    FT_GlyphSlot glyph;
} FT_FaceRec;

static const FT_Int32 NHF_FT_LOAD_NO_HINTING = 0x2;

static const char *const NHF_LIBKOBO = "/usr/local/Kobo/platforms/libkobo.so";

static FT_Error (*real_FT_Load_Glyph)(FT_Face, FT_UInt, FT_Int32);

static bool nhf_safety_disabled = false;
static bool nhf_safety_log_dumped = false;

// --- Vertical-text fix (NickelVertFix) -----------------------------------------
//
// "Better typography" (eReader.conf webkitTextRendering=optimizeLegibility) makes Nickel
// inject `text-rendering: optimizeLegibility`, which forces WebKit's *complex* text path.
// That path (QTextLayout-based) has NO vertical-writing-mode support, so vertical (tategaki)
// CJK books lose per-glyph vertical orientation — the chōonpu `ー`, brackets `「」`, and
// ideographic punctuation `、。` come out horizontal/mislaid. The *simple* path renders
// tategaki correctly. So for vertical books only, we push `text-rendering: auto` onto the
// page so it stays on the simple path. Subtractive — horizontal books are untouched.
//
// On-device tracing (see the kobo-font-investigation reports) showed why the obvious seams
// don't work: KepubBookReader::pageStyleCss/setWritingDirection are *virtual* (NickelHook can
// only patch PLT/JUMP_SLOT entries, not vtables), and ReadingSettings::getWebkitTextRendering
// is read *before* the book's vertical writing-mode is even parsed and is never re-read — so
// rewriting that value can't work. The reliable approach is to override text-rendering on the
// *live* page once vertical IS known: CustomWebView::setWritingDirection is PLT-hookable and
// its `self` exposes the page's QWebSettings, onto which we set a user stylesheet
// (`*{text-rendering:auto !important}` — user-origin !important outranks the author rule).
// All hooks/dlsyms are OPTIONAL: if a firmware rename breaks any, the vertical fix goes inert
// and the hinting fix is unaffected.

// Free function: WritingDirection writingDirectionFromString(QString const&). dlsym'd to learn
// the vertical enum values from Nickel rather than hardcoding them.
static int (*nhf_writingDirectionFromString)(const QString &) = nullptr;
// QWebSettings *CustomWebView::settings() const — the page settings to inject into.
static void *(*nhf_cwv_settings)(void *cwv) = nullptr;
// void QWebSettings::setUserStyleSheetUrl(QUrl const&) — set/clear the user stylesheet.
static void (*nhf_setUserStyleSheetUrl)(void *settings, const QUrl &url) = nullptr;
// void CustomWebView::setWritingDirection(WritingDirection) — fires once the page's writing
// mode is applied; the action seam (its self has the page settings).
static void (*real_cwv_setWritingDirection)(void *self, int dir) = nullptr;
// KepubBookReader::KepubBookReader(PluginState*, QWidget*) — fires once per book open; the
// per-book reset point. (ARM EABI ctors return `this`.)
static void *(*real_kepubReaderCtor)(void *self, void *pluginState, void *widget) = nullptr;

// `*{text-rendering:auto !important}` as a data: URL (base64).
static const char *const NHF_VERT_CSS_URL =
    "data:text/css;charset=utf-8;base64,Knt0ZXh0LXJlbmRlcmluZzphdXRvICFpbXBvcnRhbnR9";

static int  nhf_wd_vrl = -1;             // enum for "vertical-rl" (observed 3)
static int  nhf_wd_vlr = -1;             // enum for "vertical-lr" (observed 2)
static bool nhf_vertfix_ready = false;   // oracle resolved -> classification is trustworthy
static int  nhf_us_applied = -1;         // user-stylesheet state: -1 unknown, 0 cleared, 1 vertical

// --- Preferences ---------------------------------------------------------------

static bool nhf_enabled() {
    return nhf_global_config_bool("nhf_enabled", true);
}

static bool nhf_no_hinting() {
    return nhf_global_config_bool("nhf_no_hinting", true);
}

// Vertical-text fix: withhold optimizeLegibility from vertical (tategaki) books.
static bool nhf_vertfix() {
    return nhf_global_config_bool("nhf_vertfix", true);
}

// Comma-separated font families allowed to keep their own native hinting (i.e.
// exempt from nhf_no_hinting). Matched case-insensitively against the FT face.
static bool nhf_font_hinting_allowed(FT_Face face) {
    // Check the (usually empty) allow-list first. With an empty list — the
    // default — we return before ever reading face->family_name, so the common
    // path never touches the FreeType struct layout we shim.
    const char *list = nhf_global_config_get("nhf_hinting_allowlist");
    if (!list || !*list)
        return false;

    const char *family = (face && face->family_name) ? face->family_name : NULL;
    if (!family || !*family)
        return false;

    size_t flen = strlen(family);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if ((size_t)(end - start) == flen && !strncasecmp(family, start, flen))
            return true;
    }
    return false;
}

// --- Safety --------------------------------------------------------------------

static bool nhf_path_exists(const char *path) {
    return !access(path, F_OK);
}

static void nhf_write_marker(const char *path, const char *message) {
    mkdir(NHF_CONFIG_DIR, 0755);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    if (message)
        fprintf(f, "%s\n", message);
    fclose(f);
}

// Disable for this boot only — never self-uninstalls. The disabled-by-safety
// marker keeps the mod inert on later boots until the user deletes it.
static void nhf_disable_for_safety(const char *reason) {
    if (nhf_safety_disabled)
        return;
    nhf_safety_disabled = true;
    NHF_LOG("SAFETY: disabling NickelHintFix for this boot: %s", reason ? reason : "unknown reason");
    nhf_write_marker(NHF_CONFIG_DIR "/disabled-by-safety", reason);
    if (!nhf_safety_log_dumped) {
        nhf_safety_log_dumped = true;
        nh_dump_log();
    }
}

static bool nhf_safety_marker_present() {
    static int present = -1;
    if (present == -1)
        present = nhf_path_exists(NHF_CONFIG_DIR "/disabled-by-safety") ? 1 : 0;
    return present == 1;
}

// --- Init / uninstall ----------------------------------------------------------

static int nhf_init() {
    // Parse and cache config now, while we are still single-threaded at startup,
    // so the FT_Load_Glyph hook (called from multiple render threads) never races
    // to lazily build the global config. Same reason we prime the safety marker
    // cache below.
    nhf_global_config_get("");
    const char *allowlist = nhf_global_config_get("nhf_hinting_allowlist");
    NHF_LOG("startup: nhf_enabled=%d nhf_no_hinting=%d nhf_hinting_allowlist='%s'",
        nhf_enabled() ? 1 : 0,
        nhf_no_hinting() ? 1 : 0,
        allowlist ? allowlist : "");
    if (nhf_safety_marker_present())
        NHF_LOG("startup: disabled-by-safety marker present; passing through");

    // Vertical-text fix: learn the vertical-writing-mode enum values from Nickel itself
    // (version-robust). If anything is missing, the vertical fix stays inert; the hinting
    // fix is unaffected.
    NHF_LOG("startup: vertical-fix syms: cwvSetDir=%p cwvSettings=%p setUserCss=%p kepubCtor=%p wdFromString=%p",
        (void *)real_cwv_setWritingDirection, (void *)nhf_cwv_settings, (void *)nhf_setUserStyleSheetUrl,
        (void *)real_kepubReaderCtor, (void *)nhf_writingDirectionFromString);
    if (nhf_writingDirectionFromString) {
        nhf_wd_vrl = nhf_writingDirectionFromString(QStringLiteral("vertical-rl"));
        nhf_wd_vlr = nhf_writingDirectionFromString(QStringLiteral("vertical-lr"));
        nhf_vertfix_ready = true;
        NHF_LOG("startup: nhf_vertfix=%d vertical-rl=%d vertical-lr=%d",
            nhf_vertfix() ? 1 : 0, nhf_wd_vrl, nhf_wd_vlr);
    } else {
        NHF_LOG("startup: writingDirectionFromString unresolved; vertical fix inert");
    }
    return 0;
}

static bool nhf_delete_file_if_exists(const char *path) {
    if (access(path, F_OK) && errno == ENOENT)
        return true;
    return nh_delete_file(path);
}

static bool nhf_delete_dir_if_exists(const char *path) {
    if (access(path, F_OK) && errno == ENOENT)
        return true;
    return nh_delete_dir(path);
}

static bool nhf_uninstall() {
    bool ok = true;
    NHF_LOG("uninstall: removing NickelHintFix files");
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/doc") && ok;
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/default") && ok;
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/config") && ok;
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/nickelhintfix.log") && ok;
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/disabled-by-safety") && ok;
    ok = nhf_delete_file_if_exists(NHF_CONFIG_DIR "/uninstall") && ok;
    ok = nhf_delete_dir_if_exists(NHF_CONFIG_DIR) && ok;
    return ok;
}

static struct nh_info NickelHintFixInfo = {
    .name = "NickelHintFix",
    .desc = "Fix the iType grid-fit wobble (unhinted glyph loads) and broken vertical-text rendering.",
    // NickelClock-style: deleting the shipped 'uninstall' file (the uninstall_xflag)
    // triggers removal on the next boot. The uninstall_flag is a separate force path
    // that is normally absent.
    .uninstall_flag = NHF_CONFIG_DIR "/uninstall-now",
    .uninstall_xflag = NHF_CONFIG_DIR "/uninstall",
    .failsafe_delay = 3,
};

static struct nh_hook NickelHintFixHooks[] = {
    {
        .sym = "FT_Load_Glyph",
        .sym_new = "_nhf_FT_Load_Glyph",
        .lib = NHF_LIBKOBO,
        .out = nh_symoutptr(real_FT_Load_Glyph),
        .desc = "load glyphs unhinted",
    },
    // Vertical-text fix — optional so a symbol change can't break the hinting fix.
    {
        .sym = "_ZN15KepubBookReaderC1EP11PluginStateP7QWidget",
        .sym_new = "_nhf_kepubReaderCtor",
        .lib = "libnickel.so.1.0.0",
        .out = nh_symoutptr(real_kepubReaderCtor),
        .desc = "reset per-book state",
        .optional = true,
    },
    {
        .sym = "_ZN13CustomWebView19setWritingDirectionE16WritingDirection",
        .sym_new = "_nhf_cwv_setWritingDirection",
        .lib = "libnickel.so.1.0.0",
        .out = nh_symoutptr(real_cwv_setWritingDirection),
        .desc = "inject text-rendering:auto for vertical books",
        .optional = true,
    },
    {0},
};

static struct nh_dlsym NickelHintFixDlsym[] = {
    {
        .name = "_Z26writingDirectionFromStringRK7QString",
        .out = nh_symoutptr(nhf_writingDirectionFromString),
        .desc = "derive vertical enum ints at runtime",
        .optional = true,
    },
    {
        .name = "_ZNK13CustomWebView8settingsEv",
        .out = nh_symoutptr(nhf_cwv_settings),
        .desc = "reach the page's QWebSettings",
        .optional = true,
    },
    {
        .name = "_ZN12QWebSettings20setUserStyleSheetUrlERK4QUrl",
        .out = nh_symoutptr(nhf_setUserStyleSheetUrl),
        .desc = "set/clear the user stylesheet",
        .optional = true,
    },
    {0},
};

NickelHook(
    .init = &nhf_init,
    .info = &NickelHintFixInfo,
    .hook = NickelHintFixHooks,
    .dlsym = NickelHintFixDlsym,
    .uninstall = &nhf_uninstall,
)

extern "C" __attribute__((visibility("default"))) FT_Error _nhf_FT_Load_Glyph(FT_Face face, FT_UInt glyph_index, FT_Int32 load_flags) {
    if (!real_FT_Load_Glyph) {
        nhf_disable_for_safety("original FT_Load_Glyph pointer was NULL");
        return 1;
    }
    if (nhf_safety_disabled || nhf_safety_marker_present())
        return real_FT_Load_Glyph(face, glyph_index, load_flags);

    // Load uninstructed glyphs without hinting so iType draws the raw
    // outline instead of grid-fitting it. Skipped for allow-listed families.
    FT_Int32 effective_flags = load_flags;
    if (nhf_enabled() && nhf_no_hinting() && !nhf_font_hinting_allowed(face))
        effective_flags |= NHF_FT_LOAD_NO_HINTING;

    return real_FT_Load_Glyph(face, glyph_index, effective_flags);
}

// --- Vertical-text fix hooks ---------------------------------------------------

// Set (vertical=true) or clear (vertical=false) the user stylesheet on the view's page,
// forcing text-rendering:auto so vertical content stays on WebKit's simple path.
static void nhf_apply_vertical_css(void *cwv, bool vertical) {
    if (!nhf_cwv_settings || !nhf_setUserStyleSheetUrl)
        return;
    void *settings = nhf_cwv_settings(cwv);
    if (!settings)
        return;
    QUrl url;  // default-constructed (empty) clears the user stylesheet
    if (vertical)
        url = QUrl(QString::fromLatin1(NHF_VERT_CSS_URL));
    nhf_setUserStyleSheetUrl(settings, url);
}

// Per-book reset: a new KepubBookReader == a new book. Force the user-stylesheet state to be
// (re)applied on the new book's first setWritingDirection, so a previous vertical book doesn't
// leak into a horizontal one (and a new view gets the override).
extern "C" __attribute__((visibility("default")))
void *_nhf_kepubReaderCtor(void *self, void *pluginState, void *widget) {
    nhf_us_applied = -1;
    if (real_kepubReaderCtor)
        return real_kepubReaderCtor(self, pluginState, widget);
    return self;
}

// Action seam: the page's writing mode is being applied here, so vertical-ness is finally
// known AND the live page is available via the view's settings. Push (or pull) the user
// stylesheet accordingly. Guarded so we only touch WebKit when the state actually changes.
extern "C" __attribute__((visibility("default")))
void _nhf_cwv_setWritingDirection(void *self, int dir) {
    bool vert = nhf_vertfix_ready && (dir == nhf_wd_vrl || dir == nhf_wd_vlr);
    int want = vert ? 1 : 0;
    bool act = nhf_enabled() && nhf_vertfix() && nhf_vertfix_ready &&
               !nhf_safety_disabled && !nhf_safety_marker_present();
    if (act && want != nhf_us_applied) {
        nhf_apply_vertical_css(self, vert);
        nhf_us_applied = want;
    }
    if (real_cwv_setWritingDirection)
        real_cwv_setWritingDirection(self, dir);
}
