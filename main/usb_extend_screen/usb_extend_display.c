#include "usb_extend_screen_internal.h"

#include "config.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lcd.h"

#define USB_EXTEND_RGB565_SIZE (USB_EXTEND_SCREEN_WIDTH * USB_EXTEND_SCREEN_HEIGHT * 2)
#define USB_EXTEND_LCD_RGB565_SIZE (APP_LCD_H_RES * APP_LCD_V_RES * 2)
#define USB_EXTEND_LCD_BUFFER_COUNT 2
#define USB_EXTEND_LCD_SUBMIT_RETRIES 20

static const char *TAG = "usb_extend_display";
static jpeg_decoder_handle_t s_decoder;
static ppa_client_handle_t s_ppa;
static uint8_t *s_decode_output;
static uint8_t *s_lcd_output[USB_EXTEND_LCD_BUFFER_COUNT];
static uint8_t s_lcd_output_index;
static SemaphoreHandle_t s_draw_lock;

esp_err_t usb_extend_display_init(void)
{
    if (s_decoder != NULL && s_ppa != NULL && s_decode_output != NULL &&
        s_lcd_output[0] != NULL && s_lcd_output[1] != NULL && s_draw_lock != NULL) {
        return ESP_OK;
    }

    s_draw_lock = xSemaphoreCreateMutex();
    if (s_draw_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    jpeg_decode_engine_cfg_t engine_config = {
        .intr_priority = 1,
        .timeout_ms = 100,
    };
    esp_err_t err = jpeg_new_decoder_engine(&engine_config, &s_decoder);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_draw_lock);
        s_draw_lock = NULL;
        ESP_RETURN_ON_ERROR(err, TAG, "JPEG decoder init failed");
    }

    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    err = ppa_register_client(&ppa_config, &s_ppa);
    if (err != ESP_OK) {
        jpeg_del_decoder_engine(s_decoder);
        s_decoder = NULL;
        vSemaphoreDelete(s_draw_lock);
        s_draw_lock = NULL;
        ESP_RETURN_ON_ERROR(err, TAG, "PPA init failed");
    }

    size_t allocated_size = 0;
    jpeg_decode_memory_alloc_cfg_t memory_config = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    s_decode_output = jpeg_alloc_decoder_mem(USB_EXTEND_RGB565_SIZE, &memory_config, &allocated_size);
    if (s_decode_output == NULL || allocated_size < USB_EXTEND_RGB565_SIZE) {
        ppa_unregister_client(s_ppa);
        s_ppa = NULL;
        jpeg_del_decoder_engine(s_decoder);
        s_decoder = NULL;
        vSemaphoreDelete(s_draw_lock);
        s_draw_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < USB_EXTEND_LCD_BUFFER_COUNT; ++i) {
        s_lcd_output[i] = heap_caps_aligned_calloc(64, 1, USB_EXTEND_LCD_RGB565_SIZE,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        if (s_lcd_output[i] == NULL) {
            for (size_t j = 0; j < i; ++j) {
                heap_caps_free(s_lcd_output[j]);
                s_lcd_output[j] = NULL;
            }
            heap_caps_free(s_decode_output);
            s_decode_output = NULL;
            ppa_unregister_client(s_ppa);
            s_ppa = NULL;
            jpeg_del_decoder_engine(s_decoder);
            s_decoder = NULL;
            vSemaphoreDelete(s_draw_lock);
            s_draw_lock = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t usb_extend_display_draw(const uint8_t *jpeg, size_t length)
{
    if (!usb_extend_screen_active_internal()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_draw_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    jpeg_decode_cfg_t decode_config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t output_size = 0;
    uint8_t *lcd_output = s_lcd_output[s_lcd_output_index];
    esp_err_t err = jpeg_decoder_process(s_decoder, &decode_config, jpeg, length,
                                         s_decode_output, USB_EXTEND_RGB565_SIZE, &output_size);
    if (err == ESP_OK) {
        ppa_srm_oper_config_t rotate_config = {
            .in.buffer = s_decode_output,
            .in.pic_w = USB_EXTEND_SCREEN_WIDTH,
            .in.pic_h = USB_EXTEND_SCREEN_HEIGHT,
            .in.block_w = USB_EXTEND_SCREEN_WIDTH,
            .in.block_h = USB_EXTEND_SCREEN_HEIGHT,
            .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .out.buffer = lcd_output,
            .out.buffer_size = USB_EXTEND_LCD_RGB565_SIZE,
            .out.pic_w = APP_LCD_H_RES,
            .out.pic_h = APP_LCD_V_RES,
            .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
            .scale_x = 1.0f,
            .scale_y = 1.0f,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        err = ppa_do_scale_rotate_mirror(s_ppa, &rotate_config);
    }
    if (err == ESP_OK) {
        for (int retry = 0; retry < USB_EXTEND_LCD_SUBMIT_RETRIES; ++retry) {
            err = lcd_draw_rgb565_bitmap(0, 0, APP_LCD_H_RES, APP_LCD_V_RES,
                                         lcd_output);
            if (err != ESP_ERR_INVALID_STATE) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (err == ESP_OK) {
            s_lcd_output_index = (s_lcd_output_index + 1) % USB_EXTEND_LCD_BUFFER_COUNT;
        }
    }
    xSemaphoreGive(s_draw_lock);
    return err;
}

void usb_extend_display_wait_idle(void)
{
    if (s_draw_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_draw_lock, portMAX_DELAY);
    xSemaphoreGive(s_draw_lock);
}
