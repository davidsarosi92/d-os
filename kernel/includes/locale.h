/* =============================================================================
 * locale.h — the string catalogue (§M69).
 *
 * Asked for from use and never begun: every user-visible string in this system
 * is an English literal at its point of use, so the machine can type Hungarian
 * (§4.66 gave the font ISO-8859-2 and the keymap the accented vowels) and
 * cannot SAY anything in it.
 *
 * -----------------------------------------------------------------------------
 * FOUR DECISIONS, and each one is a way this could have gone wrong.
 *
 *   1. LOOKUP IS BY KEY, NOT BY INDEX.  An enum of message numbers is smaller
 *      and faster and it makes every catalogue a positional list that must be
 *      kept in step by hand — §M58's scar, one layer up: insert a string in the
 *      middle and every language after it shifts by one, silently, because
 *      nothing in a build can tell "Cancel" from "Mégse".  A key is checked at
 *      the point of use by being READ.
 *
 *   2. A MISSING STRING FALLS BACK TO ENGLISH, AND THEN TO THE KEY ITSELF —
 *      never to empty.  A half-translated interface with blank buttons is
 *      worse than an untranslated one, and a key on screen (`btn.cancel`)
 *      names exactly what has to be added.  *A gap that shows what is missing
 *      is a to-do list; a gap that shows nothing is a bug report.*
 *
 *   3. A CATALOGUE IS DATA IN A LINKER SECTION (`LOCALE_CATALOG`), the shape
 *      every other registry here uses (GUI_APP, CONFIG_KEY, ITEM_VIEW,
 *      WIDGET_CLASS).  Adding a language is a FILE, not an edit to a switch in
 *      the middle of the toolkit — which is the same swappability argument the
 *      driver, package and ABI layers were built on.
 *
 *   4. THE LANGUAGE IS A §M63 CONFIG KEY, so the Region panel renders it with
 *      no per-key UI code, and a change re-lays out every open window — a
 *      translated string is a DIFFERENT WIDTH, and a layout computed for the
 *      old one leaves controls overlapping.
 *
 * -----------------------------------------------------------------------------
 * WHAT THIS IS NOT.  There is no plural handling, no gender, no message
 * formatting with positional arguments, and no date/number formatting.  Those
 * are real and they are a different piece of work; pretending otherwise by
 * shipping a `%s`-substituting `lstrf()` would invite exactly the strings that
 * cannot be translated correctly.  Composed messages stay in code and are
 * marked as such where they occur.
 * ========================================================================= */

#ifndef LOCALE_H
#define LOCALE_H

struct locale_entry {
    const char* key;
    const char* text;
};

struct locale_catalog {
    const char* lang;                   /* "en", "hu" — matches the config    */
    const char* name;                   /* endonym, for the picker: "Magyar"  */
    const struct locale_entry* entries;
    int count;
};

/* Register a catalogue.  One per language per translation unit. */
#define LOCALE_CATALOG(_var)                                              \
    static const struct locale_catalog _var;                              \
    static const struct locale_catalog* const _var##_ptr                  \
        __attribute__((used, section("locale_catalogs"))) = &_var;         \
    static const struct locale_catalog _var

/* The translation of `key` in the active language.
 *
 * NEVER RETURNS NULL and never returns an empty string for a missing key: the
 * fallback chain is active language → English → the key itself.  Callers may
 * therefore use it directly in a draw call, which is the whole point — a
 * lookup that has to be null-checked at 300 call sites will be null-checked at
 * 290 of them. */
const char* lstr(const char* key);

/* Which language is active, and how many are registered. */
const char* locale_lang(void);
int  locale_count(void);
const struct locale_catalog* locale_at(int i);

/* Apply `lang` (called by the config watcher; also by `locale` from a shell).
 * Returns 0 if a catalogue for it exists, -1 otherwise — and REFUSING is the
 * point: silently keeping English for a language nobody registered is how a
 * setting becomes a lie. */
int locale_set(const char* lang);

/* `locale [lang]` — both shells (§M24's rule: a command that lives in one
 * shell can only be run on the arches that build it). */
void locale_cmd(const char* args);

/* Called from BOTH boot paths right after the persistent store is overlaid —
 * a language read before that is the default for one boot and the saved value
 * from the next (§M63's "one boot late"). */
void locale_init(void);

#endif /* LOCALE_H */
