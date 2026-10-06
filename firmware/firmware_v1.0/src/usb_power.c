#include <nrfx.h>
#include "usb_power.h"

bool usb_power_present(void)
{
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}
