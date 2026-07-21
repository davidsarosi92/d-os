#!/bin/sh
# =============================================================================
# build-netsurf.sh — build the NetSurf web browser (§M42) for d-os/x86_64.
#
# NetSurf's own recursive buildsystem (netsurf-buildsystem + pkg-config +
# per-frontend Makefiles) is BYPASSED.  Instead we compile a curated translation
# unit list — the core (content/desktop/utils/handlers) plus the framebuffer
# frontend + fbtk, minus JavaScript (the `none` stub set), curl, PDF, SVG, JPEG
# and WebP — against our §M42 store-lib headers, with an explicit -I set and a
# forced prelude header carrying the config macros as real string literals.  The
# result links into a musl dynamic PIE against the store .so's (resolved by ld.so
# from /lib at runtime).
#
# Runs INSIDE the d-os-build Docker container (the x86_64 musl cross toolchain is
# a Linux binary).  Invoked by `make ARCH=x86_64 netsurf`; needs the NetSurf
# sources fetched (scripts/fetch-netsurf-libs.sh netsurf) and the store libs +
# support libs (.so) already built.
# =============================================================================
set -u
# Toolchain + dynamic-linker are arch-parameterised (the Makefile `netsurf`
# target passes NS_CC/NS_LDSO per ARCH); default to x86_64.
CC=${NS_CC:-/src/third_party/musl-cross-x86_64/bin/x86_64-linux-musl-gcc}
LDSO=${NS_LDSO:-/lib/ld-musl-x86_64.so.1}
NS=/src/third_party/netsurf
TP=/src/third_party
OUT=/src/build/netsurf
U=/src/user
mkdir -p "$OUT"
rm -f "$OUT"/*.o           # arch-clean: never mix objects across toolchains

# --- generated testament.h (git version info the build normally derives) ------
cat > "$OUT/testament.h" <<'EOF'
#ifndef NETSURF_TESTAMENT_H
#define NETSURF_TESTAMENT_H
#define WT_ROOT "/src/third_party/netsurf"
#define WT_HOSTNAME "d-os"
#define WT_COMPILEDATE __DATE__
#define WT_BRANCHPATH "/"
#define WT_BRANCHISMASTER 1
#define WT_BRANCHISTRUNK 1
#define WT_BRANCHISTAG 0
#define WT_TAGIS ""
#define WT_REVID "dos-netsurf"
#define WT_MODIFIED 0
#define WT_MODIFICATIONS {}
#define WT_NO_GIT 1
#define WT_NO_SVN 1
#endif
EOF

# --- forced prelude: POSIX feature macro + config #defines (real strings) ------
# -include'd into every TU BEFORE its own headers, so _GNU_SOURCE takes effect
# and the config macros are proper string literals (no shell-quoting hell).
cat > "$OUT/dos_prelude.h" <<'EOF'
#ifndef DOS_NETSURF_PRELUDE_H
#define DOS_NETSURF_PRELUDE_H
#define _GNU_SOURCE 1
#include <time.h>
#include <dirent.h>
#include <regex.h>

#define NETSURF_HOMEPAGE "about:welcome"
#define NETSURF_FB_RESPATH "/res"
#define NETSURF_FB_FONTPATH "/res/fonts"
#define NETSURF_BUILTIN_LOG_FILTER "level:WARNING"
#define NETSURF_BUILTIN_VERBOSE_FILTER "level:VERBOSE"
#define GECOS "d-os user"
#define USERNAME "dos"

#define NETSURF_FB_FONT_SANS_SERIF             "DejaVuSans.ttf"
#define NETSURF_FB_FONT_SANS_SERIF_BOLD        "DejaVuSans-Bold.ttf"
#define NETSURF_FB_FONT_SANS_SERIF_ITALIC      "DejaVuSans-Oblique.ttf"
#define NETSURF_FB_FONT_SANS_SERIF_ITALIC_BOLD "DejaVuSans-BoldOblique.ttf"
#define NETSURF_FB_FONT_SERIF                  "DejaVuSerif.ttf"
#define NETSURF_FB_FONT_SERIF_BOLD             "DejaVuSerif-Bold.ttf"
#define NETSURF_FB_FONT_MONOSPACE              "DejaVuSansMono.ttf"
#define NETSURF_FB_FONT_MONOSPACE_BOLD         "DejaVuSansMono-Bold.ttf"
#define NETSURF_FB_FONT_CURSIVE                "DejaVuSans.ttf"
#define NETSURF_FB_FONT_FANTASY                "DejaVuSans.ttf"
#endif
EOF

# --- staging: make NetSurf's <dom/bindings/...> and <curl/curl.h> resolve ------
# libdom's hubbub binding headers live at libdom/bindings/ but NetSurf includes
# them as <dom/bindings/...> (the installed layout), so stage them under
# include/dom/bindings.  And provide a tiny stub <curl/curl.h> — content/fetch.c
# includes the curl fetcher header unconditionally, but only USES it under
# #ifdef WITH_CURL (which we leave off), so stub types satisfy the declarations.
mkdir -p "$TP/libdom/include/dom/bindings"
cp -rn "$TP/libdom/bindings/." "$TP/libdom/include/dom/bindings/" 2>/dev/null || true
mkdir -p "$OUT/stub/curl"
cat > "$OUT/stub/curl/curl.h" <<'EOF'
#ifndef DOS_CURL_STUB_H
#define DOS_CURL_STUB_H
typedef void CURL;
typedef void CURLM;
typedef int  CURLcode;
#endif
EOF

# NB: deliberately NO -I$NS/utils — it holds utils/time.h which would shadow the
# system <time.h> (angle-bracket includes also search -I dirs).  TUs reach their
# own headers as "utils/foo.h" via the root -I$NS, and same-dir "foo.h" via the
# quote-include search of the including file's directory.  Also NO
# -I$TP/libdom/bindings — it holds a hubbub/errors.h that would shadow
# libhubbub's real one (the HUBBUB_* enum); the staged include/dom/bindings copy
# above serves the <dom/bindings/...> includes instead.
INC="-I$NS -I$NS/include -I$NS/content -I$NS/content/handlers \
 -I$NS/frontends -I$NS/frontends/framebuffer -I$OUT \
 -I$TP/libwapcaplet/include -I$TP/libcss/include -I$TP/libdom/include \
 -I$TP/libhubbub/include -I$TP/libparserutils/include \
 -I$TP/libnsutils/include -I$TP/libnslog/include -I$TP/libnsgif/include \
 -I$TP/libnsbmp/include -I$TP/libnsfb/include -I$TP/libnspsl/include \
 -I$TP/zlib -I$TP/libpng -I$TP/freetype/include -I$OUT/stub"

DEFS="-Os -w -std=gnu99 -fPIC -D_GNU_SOURCE -Dnsframebuffer -Dsmall \
 -DFB_USE_FREETYPE -include $OUT/dos_prelude.h"

# --- curated source list ------------------------------------------------------
S_CONTENT="content.c content_factory.c fetch.c hlcache.c llcache.c mimesniff.c textsearch.c urldb.c no_backing_store.c"
S_FETCH="fetchers/data.c fetchers/resource.c fetchers/file/dirlist.c fetchers/file/file.c"
S_FETCH_ABOUT="about.c blank.c certificate.c chart.c choices.c config.c imagecache.c nscolours.c query.c query_auth.c query_fetcherror.c query_privacy.c query_timeout.c testament.c websearch.c"
S_UTILS="bloom.c corestrings.c file.c filepath.c hashmap.c hashtable.c idna.c libdom.c log.c messages.c nscolour.c nsoption.c punycode.c ssl_certs.c talloc.c time.c url.c useragent.c utf8.c utils.c"
S_HTTP="challenge.c generics.c primitives.c parameter.c cache-control.c content-disposition.c content-type.c strict-transport-security.c www-authenticate.c"
S_NSURL="nsurl.c parse.c"
S_DESKTOP="cookie_manager.c knockout.c hotlist.c mouse.c plot_style.c print.c search.c searchweb.c scrollbar.c textarea.c version.c system_colour.c local_history.c global_history.c treeview.c page-info.c"
S_BROWSER="bitmap.c browser.c browser_window.c browser_history.c download.c frames.c netsurf.c cw_helper.c save_complete.c save_text.c selection.c textinput.c gui_factory.c save_pdf.c font_haru.c"
S_CSS="css.c dump.c internal.c hints.c select.c"
S_HTML="box_construct.c box_inspect.c box_manipulate.c box_normalise.c box_special.c box_textarea.c css.c css_fetcher.c dom_event.c font.c form.c forms.c html.c imagemap.c interaction.c layout.c layout_flex.c object.c redraw.c redraw_border.c script.c table.c textselection.c"
S_TEXT="textplain.c"
S_JS="none/none.c fetcher.c"
S_IMAGE="image.c image_cache.c bmp.c gif.c ico.c png.c"
S_FRONTEND="gui.c framebuffer.c schedule.c bitmap.c fetch.c findfile.c corewindow.c local_history.c clipboard.c font_freetype.c"
S_FBTK="fbtk.c event.c fill.c bitmap.c user.c window.c text.c scroll.c osk.c"

SRCS=""
for f in $S_CONTENT;     do SRCS="$SRCS $NS/content/$f"; done
for f in $S_FETCH;       do SRCS="$SRCS $NS/content/$f"; done
for f in $S_FETCH_ABOUT; do SRCS="$SRCS $NS/content/fetchers/about/$f"; done
for f in $S_UTILS;       do SRCS="$SRCS $NS/utils/$f"; done
for f in $S_HTTP;        do SRCS="$SRCS $NS/utils/http/$f"; done
for f in $S_NSURL;       do SRCS="$SRCS $NS/utils/nsurl/$f"; done
for f in $S_DESKTOP;     do SRCS="$SRCS $NS/desktop/$f"; done
for f in $S_BROWSER;     do SRCS="$SRCS $NS/desktop/$f"; done
for f in $S_CSS;         do SRCS="$SRCS $NS/content/handlers/css/$f"; done
for f in $S_HTML;        do SRCS="$SRCS $NS/content/handlers/html/$f"; done
for f in $S_TEXT;        do SRCS="$SRCS $NS/content/handlers/text/$f"; done
for f in $S_JS;          do SRCS="$SRCS $NS/content/handlers/javascript/$f"; done
for f in $S_IMAGE;       do SRCS="$SRCS $NS/content/handlers/image/$f"; done
for f in $S_FRONTEND;    do SRCS="$SRCS $NS/frontends/framebuffer/$f"; done
for f in $S_FBTK;        do SRCS="$SRCS $NS/frontends/framebuffer/fbtk/$f"; done
# d-os glue: ABI-compatible stubs for the built-in toolbar/cursor/throbber
# bitmaps (upstream generated by convert_image; not on the first-render path).
SRCS="$SRCS $U/netsurf/dos_image_data.c"

# --- compile ------------------------------------------------------------------
FAIL=0; OK=0
for src in $SRCS; do
    obj="$OUT/$(echo "$src" | sed 's,/,_,g').o"
    if ! $CC -c $DEFS $INC "$src" -o "$obj" 2>"$OUT/err.txt"; then
        FAIL=$((FAIL+1)); echo "FAIL: $src"; head -5 "$OUT/err.txt"; echo "---"
    else
        OK=$((OK+1))
    fi
done
echo "netsurf: compiled OK=$OK FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "netsurf: compile failed"; exit 1; }

# --- link: musl dynamic PIE against the store .so's (by path) -----------------
$CC -pie -o "$OUT/netsurf.dynelf" "$OUT"/*.o \
    -Wl,-dynamic-linker,"$LDSO" -Wl,-rpath-link,"$U" \
    "$U/libcss.so.0" "$U/libdom.so.0" "$U/libhubbub.so.0" \
    "$U/libwapcaplet.so.0" "$U/libparserutils.so.0" \
    "$U/libnsutils.so.0" "$U/libnslog.so.0" "$U/libnspsl.so.0" \
    "$U/libnsgif.so.0" "$U/libnsbmp.so.0" "$U/libnsfb.so.0" \
    "$U/libpng16.so.16" "$U/libz.so.1" "$U/libfreetype.so.6" -lm || {
        echo "netsurf: link failed"; exit 1; }

cp "$OUT/netsurf.dynelf" "$U/netsurf.dynelf"
echo "netsurf: linked -> user/netsurf.dynelf ($(wc -c < "$OUT/netsurf.dynelf") bytes)"
