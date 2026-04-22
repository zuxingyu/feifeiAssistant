/*******************************************************************************
 * Size: 24 px
 * Bpp: 1
 * Opts: --force-fast-kern-format --no-compress --no-prefilter --font /Users/zuxingyu/Downloads/AlibabaPuHuiTi-3-55-Regular/AlibabaPuHuiTi-3-55-Regular.ttf --format lvgl --lv-include lvgl.h --bpp 1 -o /Users/zuxingyu/Documents/workspace/esp/feifeiAssistant/main/boards/waveshare/esp32-s3-rlcd-4.2/assets/fonts/alibaba_puhui_title_24.c --size 24 --symbols 首次配网
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef ALIBABA_PUHUI_TITLE_24
#define ALIBABA_PUHUI_TITLE_24 1
#endif

#if ALIBABA_PUHUI_TITLE_24

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+6B21 "次" */
    0x0, 0x60, 0x2, 0x3, 0x0, 0x38, 0x18, 0x0,
    0xc1, 0x80, 0x3, 0xf, 0xff, 0x1c, 0x7f, 0xf8,
    0x66, 0x1, 0x81, 0x33, 0xc, 0x3, 0x18, 0xc0,
    0x18, 0xc6, 0x0, 0x86, 0x20, 0x30, 0x30, 0x1,
    0x81, 0x80, 0x18, 0x1e, 0x0, 0xc0, 0xf0, 0x6,
    0xe, 0xc0, 0x60, 0x67, 0x3, 0x6, 0x1c, 0x18,
    0x60, 0x71, 0x8e, 0x1, 0xcc, 0xe0, 0x7, 0x84,
    0x0, 0x8,

    /* U+7F51 "网" */
    0xff, 0xff, 0xf0, 0x0, 0x3c, 0x0, 0xf, 0x42,
    0xb, 0xd9, 0xa6, 0xf3, 0x69, 0xbc, 0xd3, 0x4f,
    0x1c, 0x73, 0xc6, 0x1c, 0xf1, 0x86, 0x3c, 0x71,
    0xcf, 0x34, 0xd3, 0xcd, 0xb6, 0xf6, 0x79, 0xbd,
    0x86, 0x3f, 0xc3, 0xf, 0xd0, 0xc0, 0xf0, 0x0,
    0x3c, 0x0, 0xff, 0x0, 0x1e,

    /* U+914D "配" */
    0xff, 0xef, 0xf0, 0x58, 0x1, 0x82, 0xc0, 0xc,
    0x16, 0x0, 0x67, 0xfc, 0x3, 0x25, 0xa0, 0x19,
    0x2d, 0x0, 0xcb, 0x69, 0xfe, 0x5b, 0x4c, 0x2,
    0x9a, 0x60, 0x1c, 0x73, 0x0, 0x80, 0x98, 0x4,
    0x4, 0xc0, 0x3f, 0xe6, 0xd, 0x1, 0x30, 0x68,
    0x9, 0x83, 0x40, 0x4c, 0x1a, 0x2, 0x60, 0xdf,
    0xf1, 0xfc,

    /* U+9996 "首" */
    0x6, 0x3, 0x0, 0x18, 0x38, 0x0, 0xc1, 0x81,
    0xff, 0xff, 0xf0, 0x6, 0x0, 0x0, 0x30, 0x0,
    0x1, 0x80, 0x3, 0xff, 0xf8, 0x18, 0x0, 0xc0,
    0xc0, 0x6, 0x6, 0x0, 0x30, 0x3f, 0xff, 0x81,
    0x80, 0xc, 0xc, 0x0, 0x60, 0x60, 0x3, 0x3,
    0xff, 0xf8, 0x18, 0x0, 0xc0, 0xc0, 0x6, 0x6,
    0x0, 0x30, 0x3f, 0xff, 0x81, 0x80, 0xc, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 378, .box_w = 21, .box_h = 22, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 58, .adv_w = 378, .box_w = 18, .box_h = 20, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 103, .adv_w = 378, .box_w = 21, .box_h = 19, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 153, .adv_w = 378, .box_w = 21, .box_h = 21, .ofs_x = 1, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x1430, 0x262c, 0x2e75
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 27425, .range_length = 11894, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 4, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t alibaba_puhui_title_24 = {
#else
lv_font_t alibaba_puhui_title_24 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 22,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -2,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if ALIBABA_PUHUI_TITLE_24*/

