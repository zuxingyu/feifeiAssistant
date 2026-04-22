/* Bold black-and-white battery icon for RLCD visibility */
#include "lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#define PX_W 0xff, 0xff
#define PX_B 0x00, 0x00

const LV_ATTRIBUTE_MEM_ALIGN uint8_t ui_img_battery_visible_map[] = {
  PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_B, PX_B, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_B, PX_B, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_B, PX_B, PX_W, PX_B, PX_B, PX_B, PX_B, PX_W, PX_B, PX_B, PX_B, PX_B, PX_W, PX_W,
  PX_W, PX_W, PX_B, PX_B, PX_W, PX_B, PX_B, PX_B, PX_B, PX_W, PX_B, PX_B, PX_B, PX_B, PX_W, PX_W,
  PX_W, PX_W, PX_B, PX_B, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_B, PX_B, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_B, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W,
  PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W, PX_W,
};

const lv_image_dsc_t ui_img_battery_visible = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_RGB565,
  .header.w = 16,
  .header.h = 16,
  .header.stride = 32,
  .data_size = 512,
  .data = ui_img_battery_visible_map,
};
