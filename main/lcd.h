#ifndef LCD_H
#define LCD_H

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

void lcd_init(void);
esp_lcd_panel_io_handle_t lcd_get_panel_io_handle(void);
esp_lcd_panel_handle_t lcd_get_panel_handle(void);
esp_lcd_touch_handle_t lcd_get_touch_handle(void);
esp_err_t lcd_draw_rgb565_bitmap(int x_start, int y_start, int x_end, int y_end, const void *color_data);
esp_err_t lcd_set_backlight_percent(int percent);
int lcd_get_backlight_percent(void);

#endif
