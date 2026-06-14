#include <string.h>

#include "tusb.h"
#include "usb_extend_hid_descriptor.h"
#include "usb_extend_screen_internal.h"

#define USB_EXTEND_VID 0x303A
#define USB_EXTEND_PID 0x2986

enum {
    INTERFACE_VENDOR,
    INTERFACE_HID,
    INTERFACE_TOTAL,
};

static const uint8_t s_hid_report_descriptor[] = {
    USB_EXTEND_HID_REPORT_DESC_TOUCH_SCREEN(USB_EXTEND_REPORT_ID_TOUCH,
                                            USB_EXTEND_SCREEN_WIDTH,
                                            USB_EXTEND_SCREEN_HEIGHT),
};

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceProtocol = TUSB_CLASS_UNSPECIFIED,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_EXTEND_VID,
    .idProduct = USB_EXTEND_PID,
    .bcdDevice = 0x0101,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, INTERFACE_TOTAL, 0,
                          TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_HID_DESC_LEN,
                          0, 100),
    TUD_VENDOR_DESCRIPTOR(INTERFACE_VENDOR, 4, 0x01, 0x81, CFG_TUD_VENDOR_EPSIZE),
    TUD_HID_DESCRIPTOR(INTERFACE_HID, 5, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor), 0x82,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
};

static const char *s_string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "Espressif",
    "ESP Extend Screen",
    "012-2021",
    "esp32p4udisp0_R800x480_Ejpg6_Fps30_Bl300000",
    "touch",
};
static uint16_t s_string_buffer[64];

uint8_t const *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_device_descriptor;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_configuration_descriptor;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t language_id)
{
    (void)language_id;
    if (index >= sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0])) {
        return NULL;
    }

    uint8_t count = 0;
    if (index == 0) {
        memcpy(&s_string_buffer[1], s_string_descriptors[0], 2);
        count = 1;
    } else {
        const char *text = s_string_descriptors[index];
        count = (uint8_t)strlen(text);
        if (count > 63) {
            count = 63;
        }
        for (uint8_t i = 0; i < count; ++i) {
            s_string_buffer[i + 1] = text[i];
        }
    }
    s_string_buffer[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return s_string_buffer;
}
