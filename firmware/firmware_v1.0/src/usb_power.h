#pragma once

#include <stdbool.h>

// USB 給電（VBUS）の有無を返す。nRF52840 の POWER.USBREGSTATUS.VBUSDETECT を直接読むので
// USB スタック（CONFIG_USB_DEVICE_STACK）を有効にしなくても使える。
bool usb_power_present(void);
