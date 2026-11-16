#pragma once

/*
 * Minimal BLE HID profile that behaves like a cheap "AB Shutter3" remote:
 *  - advertises as "AB Shutter3" with the HID service
 *  - pairs WITHOUT a PIN ("Just Works"), which is what e-readers, phones and
 *    cameras expect from such a remote. The stock Flipper HID profile always asks for a PIN
 *    confirmation, which such devices cannot do -> pairing hangs.
 */

#include <furi_ble/profile_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHUTTER_DEVICE_NAME "AB Flipper"

extern const FuriHalBleProfileTemplate* shutter_profile;

bool shutter_profile_key_press(FuriHalBleProfileBase* profile, uint8_t keycode);
bool shutter_profile_key_release(FuriHalBleProfileBase* profile);
bool shutter_profile_consumer_press(FuriHalBleProfileBase* profile, uint16_t usage);
bool shutter_profile_consumer_release(FuriHalBleProfileBase* profile);
void shutter_profile_release_all(FuriHalBleProfileBase* profile);

#ifdef __cplusplus
}
#endif
