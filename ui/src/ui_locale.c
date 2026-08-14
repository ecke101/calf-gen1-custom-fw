#include "ui_internal.h"

typedef struct {
    const char *english;
    const char *text[CALF_LANGUAGE_COUNT];
} locale_entry_t;

/* English source text is the stable lookup key. Add translated entries to
 * this catalog as languages are introduced. The empty entry keeps the
 * English-only catalog valid C while making its future table shape explicit. */
static const locale_entry_t k_locale[] = {
    {"", {""}},
};

/* Every string reaches this function at the renderer boundary, including
 * notices produced by the target. Adding a language consists of extending
 * calf_language_t and k_languages, then filling its k_locale column. Dynamic
 * values that contain words should be assembled from translated fragments
 * when the first non-English catalog is introduced. */
const char *calf_ui_translate(calf_language_t language, const char *english)
{
    size_t index;
    const char *translated;
    if(english == (const char *)0) return "";
    if((int)language < 0 || language >= CALF_LANGUAGE_COUNT)
        language = CALF_LANGUAGE_ENGLISH;
    for(index = 0; index < ARRAY_SIZE(k_locale); ++index) {
        if(!text_equal(k_locale[index].english, english)) continue;
        translated = k_locale[index].text[language];
        return translated != (const char *)0 ? translated : english;
    }
    return english;
}
