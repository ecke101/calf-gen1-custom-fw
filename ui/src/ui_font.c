#include "ui_internal.h"

#if __STDC_HOSTED__
#include <math.h>
#include <stdlib.h>
#else
extern void *malloc(size_t size);
extern void free(void *pointer);
extern double floor(double value);
extern double ceil(double value);
extern double sqrt(double value);
extern double pow(double base, double exponent);
extern double fmod(double numerator, double denominator);
extern double cos(double value);
extern double acos(double value);
extern double fabs(double value);
#endif

static void *font_memcpy(void *destination, const void *source, size_t count)
{
    size_t index;
    unsigned char *output = (unsigned char *)destination;
    const unsigned char *input = (const unsigned char *)source;
    for(index = 0; index < count; ++index) output[index] = input[index];
    return destination;
}

static void *font_memset(void *destination, int value, size_t count)
{
    size_t index;
    unsigned char *output = (unsigned char *)destination;
    for(index = 0; index < count; ++index) output[index] = (unsigned char)value;
    return destination;
}

static size_t font_strlen(const char *text)
{
    return text_length(text);
}

#define STBTT_ifloor(value) ((int)floor((double)(value)))
#define STBTT_iceil(value) ((int)ceil((double)(value)))
#define STBTT_sqrt(value) sqrt((double)(value))
#define STBTT_pow(base, exponent) pow((double)(base), (double)(exponent))
#define STBTT_fmod(numerator, denominator) \
    fmod((double)(numerator), (double)(denominator))
#define STBTT_cos(value) cos((double)(value))
#define STBTT_acos(value) acos((double)(value))
#define STBTT_fabs(value) fabs((double)(value))
#define STBTT_malloc(size, user) ((void)(user), malloc((size_t)(size)))
#define STBTT_free(pointer, user) ((void)(user), free(pointer))
#define STBTT_assert(condition) ((void)sizeof(condition))
#define STBTT_strlen(text) font_strlen(text)
#define STBTT_memcpy font_memcpy
#define STBTT_memset font_memset
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb/stb_truetype.h"

extern const unsigned char calf_font_noto_sans[];
extern const size_t calf_font_noto_sans_size;
extern const unsigned char calf_font_noto_symbols[];
extern const size_t calf_font_noto_symbols_size;
extern const unsigned char calf_font_noto_symbols2[];
extern const size_t calf_font_noto_symbols2_size;

#define FONT_COUNT 3
#define FONT_CACHE_COUNT 512
#define FONT_CACHE_DIMENSION 48

typedef struct {
    stbtt_fontinfo info;
    int ascent;
    int descent;
    int line_gap;
    int valid;
} loaded_font_t;

typedef struct {
    uint32_t codepoint;
    uint32_t last_used;
    int advance;
    int x_offset;
    int y_offset;
    int baseline;
    int width;
    int height;
    unsigned char pixel_height;
    unsigned char font_index;
    unsigned char valid;
    unsigned char bitmap[FONT_CACHE_DIMENSION * FONT_CACHE_DIMENSION];
} cached_glyph_t;

static loaded_font_t g_fonts[FONT_COUNT];
static cached_glyph_t g_glyph_cache[FONT_CACHE_COUNT];
static uint32_t g_cache_clock;
static int g_fonts_initialized;
static int g_fonts_valid;

static int rounded_float(float value)
{
    return value < 0.0f ? (int)(value - 0.5f) : (int)(value + 0.5f);
}

static int font_pixel_height(int scale)
{
    static const unsigned char heights[] = {0u, 18u, 22u, 30u, 36u};
    if(scale <= 0) return 0;
    if(scale < (int)ARRAY_SIZE(heights)) return heights[scale];
    return scale * 8 + 2;
}

static void initialize_fonts(void)
{
    static const unsigned char *const data[FONT_COUNT] = {
        calf_font_noto_sans,
        calf_font_noto_symbols,
        calf_font_noto_symbols2,
    };
    static const size_t *const sizes[FONT_COUNT] = {
        &calf_font_noto_sans_size,
        &calf_font_noto_symbols_size,
        &calf_font_noto_symbols2_size,
    };
    int index;
    if(g_fonts_initialized) return;
    g_fonts_initialized = 1;
    g_fonts_valid = 1;
    for(index = 0; index < FONT_COUNT; ++index) {
        int offset;
        if(*sizes[index] < 12u) {
            g_fonts_valid = 0;
            continue;
        }
        offset = stbtt_GetFontOffsetForIndex(data[index], 0);
        if(offset < 0 || !stbtt_InitFont(&g_fonts[index].info,
                                        data[index], offset)) {
            g_fonts_valid = 0;
            continue;
        }
        stbtt_GetFontVMetrics(&g_fonts[index].info,
                             &g_fonts[index].ascent,
                             &g_fonts[index].descent,
                             &g_fonts[index].line_gap);
        g_fonts[index].valid = 1;
    }
}

static int font_for_codepoint(uint32_t codepoint)
{
    int index;
    initialize_fonts();
    if(!g_fonts_valid) return -1;
    for(index = 0; index < FONT_COUNT; ++index) {
        if(g_fonts[index].valid &&
           stbtt_FindGlyphIndex(&g_fonts[index].info, (int)codepoint) != 0)
            return index;
    }
    return 0;
}

static uint32_t next_codepoint(const char **cursor)
{
    const unsigned char *text = (const unsigned char *)*cursor;
    uint32_t codepoint;
    if(text[0] == 0u) return 0u;
    if(text[0] < 0x80u) {
        *cursor += 1;
        return text[0];
    }
    if(text[0] >= 0xc2u && text[0] <= 0xdfu &&
       (text[1] & 0xc0u) == 0x80u) {
        codepoint = ((uint32_t)(text[0] & 0x1fu) << 6) |
                    (uint32_t)(text[1] & 0x3fu);
        *cursor += 2;
        return codepoint;
    }
    if(text[0] >= 0xe0u && text[0] <= 0xefu && text[1] != 0u &&
       text[2] != 0u && (text[1] & 0xc0u) == 0x80u &&
       (text[2] & 0xc0u) == 0x80u &&
       !(text[0] == 0xe0u && text[1] < 0xa0u) &&
       !(text[0] == 0xedu && text[1] >= 0xa0u)) {
        codepoint = ((uint32_t)(text[0] & 0x0fu) << 12) |
                    ((uint32_t)(text[1] & 0x3fu) << 6) |
                    (uint32_t)(text[2] & 0x3fu);
        *cursor += 3;
        return codepoint;
    }
    if(text[0] >= 0xf0u && text[0] <= 0xf4u && text[1] != 0u &&
       text[2] != 0u && text[3] != 0u &&
       (text[1] & 0xc0u) == 0x80u && (text[2] & 0xc0u) == 0x80u &&
       (text[3] & 0xc0u) == 0x80u &&
       !(text[0] == 0xf0u && text[1] < 0x90u) &&
       !(text[0] == 0xf4u && text[1] >= 0x90u)) {
        codepoint = ((uint32_t)(text[0] & 0x07u) << 18) |
                    ((uint32_t)(text[1] & 0x3fu) << 12) |
                    ((uint32_t)(text[2] & 0x3fu) << 6) |
                    (uint32_t)(text[3] & 0x3fu);
        *cursor += 4;
        return codepoint;
    }
    *cursor += 1;
    return 0xfffdu;
}

static uint32_t cache_time(void)
{
    size_t index;
    ++g_cache_clock;
    if(g_cache_clock != 0u) return g_cache_clock;
    for(index = 0; index < ARRAY_SIZE(g_glyph_cache); ++index)
        g_glyph_cache[index].last_used = 0u;
    g_cache_clock = 1u;
    return g_cache_clock;
}

static cached_glyph_t *cached_glyph(uint32_t codepoint, int pixel_height,
                                    int font_index)
{
    cached_glyph_t *candidate = &g_glyph_cache[0];
    uint32_t now = cache_time();
    size_t index;
    float font_scale;
    int advance;
    int left_bearing;
    int x0;
    int y0;
    int x1;
    int y1;
    for(index = 0; index < ARRAY_SIZE(g_glyph_cache); ++index) {
        cached_glyph_t *entry = &g_glyph_cache[index];
        if(entry->valid && entry->codepoint == codepoint &&
           entry->pixel_height == pixel_height &&
           entry->font_index == font_index) {
            entry->last_used = now;
            return entry;
        }
        if(!entry->valid || entry->last_used < candidate->last_used)
            candidate = entry;
    }

    font_scale = stbtt_ScaleForPixelHeight(&g_fonts[font_index].info,
                                           (float)pixel_height);
    stbtt_GetCodepointHMetrics(&g_fonts[font_index].info, (int)codepoint,
                              &advance, &left_bearing);
    (void)left_bearing;
    stbtt_GetCodepointBitmapBox(&g_fonts[font_index].info, (int)codepoint,
                                font_scale, font_scale, &x0, &y0, &x1, &y1);
    candidate->codepoint = codepoint;
    candidate->last_used = now;
    candidate->advance = rounded_float((float)advance * font_scale);
    candidate->x_offset = x0;
    candidate->y_offset = y0;
    candidate->baseline = rounded_float(
        (float)g_fonts[font_index].ascent * font_scale);
    candidate->width = x1 - x0;
    candidate->height = y1 - y0;
    candidate->pixel_height = (unsigned char)pixel_height;
    candidate->font_index = (unsigned char)font_index;
    candidate->valid = 1u;
    font_memset(candidate->bitmap, 0, sizeof(candidate->bitmap));
    if(candidate->width > 0 && candidate->height > 0 &&
       candidate->width <= FONT_CACHE_DIMENSION &&
       candidate->height <= FONT_CACHE_DIMENSION) {
        stbtt_MakeCodepointBitmap(&g_fonts[font_index].info,
                                  candidate->bitmap,
                                  candidate->width, candidate->height,
                                  FONT_CACHE_DIMENSION, font_scale, font_scale,
                                  (int)codepoint);
    }
    else {
        candidate->width = 0;
        candidate->height = 0;
    }
    return candidate;
}

static int codepoint_kerning(uint32_t left, int left_font,
                             uint32_t right, int right_font,
                             int pixel_height)
{
    float scale;
    int units;
    if(left == 0u || left_font != right_font || left_font < 0) return 0;
    scale = stbtt_ScaleForPixelHeight(&g_fonts[left_font].info,
                                      (float)pixel_height);
    units = stbtt_GetCodepointKernAdvance(&g_fonts[left_font].info,
                                         (int)left, (int)right);
    return rounded_float((float)units * scale);
}

static int codepoint_advance(uint32_t codepoint, int font_index,
                             int pixel_height)
{
    float scale = stbtt_ScaleForPixelHeight(&g_fonts[font_index].info,
                                            (float)pixel_height);
    int advance;
    int left_bearing;
    stbtt_GetCodepointHMetrics(&g_fonts[font_index].info, (int)codepoint,
                              &advance, &left_bearing);
    (void)left_bearing;
    return rounded_float((float)advance * scale);
}

static uint32_t blend_coverage(uint32_t destination, uint32_t source,
                               unsigned coverage)
{
    uint32_t source_alpha = ((source >> 24) & 0xffu) * coverage / 255u;
    uint32_t destination_alpha = (destination >> 24) & 0xffu;
    uint32_t inverse_source = 255u - source_alpha;
    uint32_t output_alpha =
        source_alpha + destination_alpha * inverse_source / 255u;
    uint32_t output = output_alpha << 24;
    int shift;
    if(source_alpha == 0u) return destination;
    if(output_alpha == 0u) return 0u;
    for(shift = 0; shift <= 16; shift += 8) {
        uint32_t source_component = (source >> shift) & 0xffu;
        uint32_t destination_component = (destination >> shift) & 0xffu;
        uint32_t premultiplied =
            source_component * source_alpha +
            destination_component * destination_alpha * inverse_source / 255u;
        uint32_t component = premultiplied / output_alpha;
        output |= component << shift;
    }
    return output;
}

int calf_font_text_width(const char *text, int scale)
{
    const char *cursor = text;
    uint32_t previous = 0u;
    int previous_font = -1;
    int pixel_height = font_pixel_height(scale);
    int width = 0;
    if(text == (const char *)0 || scale <= 0) return 0;
    initialize_fonts();
    if(!g_fonts_valid) return 0;
    while(*cursor != '\0') {
        uint32_t codepoint = next_codepoint(&cursor);
        int font_index = font_for_codepoint(codepoint);
        if(font_index < 0) return 0;
        width += codepoint_kerning(previous, previous_font, codepoint,
                                   font_index, pixel_height);
        width += codepoint_advance(codepoint, font_index, pixel_height);
        previous = codepoint;
        previous_font = font_index;
    }
    return width;
}

int calf_font_text_height(int scale)
{
    return font_pixel_height(scale);
}

int calf_font_has_codepoint(uint32_t codepoint)
{
    int index;
    initialize_fonts();
    if(!g_fonts_valid) return 0;
    for(index = 0; index < FONT_COUNT; ++index) {
        if(g_fonts[index].valid &&
           stbtt_FindGlyphIndex(&g_fonts[index].info, (int)codepoint) != 0)
            return 1;
    }
    return 0;
}

void calf_font_draw(uint32_t *pixels, int stride, int x, int y,
                    const char *text, int scale, uint32_t color)
{
    const char *cursor = text;
    uint32_t previous = 0u;
    int previous_font = -1;
    int pixel_height = font_pixel_height(scale);
    int pen_x = x;
    if(pixels == (uint32_t *)0 || text == (const char *)0 ||
       stride <= 0 || scale <= 0)
        return;
    initialize_fonts();
    if(!g_fonts_valid) return;
    while(*cursor != '\0') {
        uint32_t codepoint = next_codepoint(&cursor);
        int font_index = font_for_codepoint(codepoint);
        cached_glyph_t *glyph;
        int row;
        if(font_index < 0) return;
        glyph = cached_glyph(codepoint, pixel_height, font_index);
        pen_x += codepoint_kerning(previous, previous_font, codepoint,
                                   font_index, pixel_height);
        for(row = 0; row < glyph->height; ++row) {
            int destination_y = y + glyph->baseline + glyph->y_offset + row;
            int column;
            if(destination_y < 0 || destination_y >= CALF_UI_HEIGHT) continue;
            for(column = 0; column < glyph->width; ++column) {
                int destination_x = pen_x + glyph->x_offset + column;
                unsigned coverage =
                    glyph->bitmap[row * FONT_CACHE_DIMENSION + column];
                uint32_t *destination;
                if(coverage == 0u || destination_x < 0 ||
                   destination_x >= CALF_UI_WIDTH)
                    continue;
                destination = &pixels[destination_y * stride + destination_x];
                *destination = blend_coverage(*destination, color, coverage);
            }
        }
        pen_x += glyph->advance;
        previous = codepoint;
        previous_font = font_index;
    }
}
