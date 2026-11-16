#include "shutter_profile.h"

#include <furi.h>
#include <furi_hal.h>
#include <services/dev_info_service.h>
#include <services/battery_service.h>
#include <extra_services/hid_service.h>

// Values from the Bluetooth spec (defined here so we don't depend on stack headers)
#define SHUTTER_UUID_TYPE_16 (0x01)
#define SHUTTER_HID_SERVICE_UUID (0x1812)
#define SHUTTER_APPEARANCE_KEYBOARD (0x03C1)

// The HID service of the firmware always has 3 input reports.
// Report IDs are fixed to index + 1, so our report map must match that order.
enum {
    ReportNumberKeyboard = 0, // Report ID 1
    ReportNumberMouse = 1, // Report ID 2 (unused, kept so the layout matches)
    ReportNumberConsumer = 2, // Report ID 3
};

typedef struct {
    uint8_t mods;
    uint8_t reserved;
    uint8_t key[6];
} FURI_PACKED ShutterKbReport;

typedef struct {
    uint16_t usage;
} FURI_PACKED ShutterConsumerReport;

// Keyboard (ID 1) + Mouse (ID 2) + Consumer Control (ID 3), written as raw bytes
static const uint8_t shutter_report_map[] = {
    // ---- Keyboard, Report ID 1 ----
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x06, // Usage (Keyboard)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x01, //   Report ID (1)
    0x05, 0x07, //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0, //   Usage Minimum (Left Ctrl)
    0x29, 0xE7, //   Usage Maximum (Right GUI)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x08, //   Report Count (8)
    0x81, 0x02, //   Input (Data, Variable, Absolute)  - modifiers
    0x95, 0x01, //   Report Count (1)
    0x75, 0x08, //   Report Size (8)
    0x81, 0x03, //   Input (Constant)                   - reserved byte
    0x05, 0x08, //   Usage Page (LEDs)
    0x95, 0x08, //   Report Count (8)
    0x75, 0x01, //   Report Size (1)
    0x19, 0x01, //   Usage Minimum (1)
    0x29, 0x08, //   Usage Maximum (8)
    0x91, 0x02, //   Output (Data, Variable, Absolute)  - LEDs
    0x95, 0x06, //   Report Count (6)
    0x75, 0x08, //   Report Size (8)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x65, //   Logical Maximum (101)
    0x05, 0x07, //   Usage Page (Keyboard/Keypad)
    0x19, 0x00, //   Usage Minimum (0)
    0x29, 0x65, //   Usage Maximum (101)
    0x81, 0x00, //   Input (Data, Array)                - 6 keys
    0xC0, // End Collection

    // ---- Mouse, Report ID 2 (never sent) ----
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x02, // Usage (Mouse)
    0xA1, 0x01, // Collection (Application)
    0x09, 0x01, //   Usage (Pointer)
    0xA1, 0x00, //   Collection (Physical)
    0x85, 0x02, //     Report ID (2)
    0x05, 0x09, //     Usage Page (Buttons)
    0x19, 0x01, //     Usage Minimum (1)
    0x29, 0x03, //     Usage Maximum (3)
    0x15, 0x00, //     Logical Minimum (0)
    0x25, 0x01, //     Logical Maximum (1)
    0x95, 0x03, //     Report Count (3)
    0x75, 0x01, //     Report Size (1)
    0x81, 0x02, //     Input (Data, Variable, Absolute)
    0x75, 0x01, //     Report Size (1)
    0x95, 0x05, //     Report Count (5)
    0x81, 0x03, //     Input (Constant)
    0x05, 0x01, //     Usage Page (Generic Desktop)
    0x09, 0x30, //     Usage (X)
    0x09, 0x31, //     Usage (Y)
    0x09, 0x38, //     Usage (Wheel)
    0x15, 0x81, //     Logical Minimum (-127)
    0x25, 0x7F, //     Logical Maximum (127)
    0x75, 0x08, //     Report Size (8)
    0x95, 0x03, //     Report Count (3)
    0x81, 0x06, //     Input (Data, Variable, Relative)
    0xC0, //   End Collection
    0xC0, // End Collection

    // ---- Consumer Control, Report ID 3 (Volume Up/Down like the AB Shutter3) ----
    0x05, 0x0C, // Usage Page (Consumer)
    0x09, 0x01, // Usage (Consumer Control)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x03, //   Report ID (3)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x03, //   Logical Maximum (0x3FF)
    0x19, 0x00, //   Usage Minimum (0)
    0x2A, 0xFF, 0x03, //   Usage Maximum (0x3FF)
    0x95, 0x01, //   Report Count (1)
    0x75, 0x10, //   Report Size (16)
    0x81, 0x00, //   Input (Data, Array)
    0xC0, // End Collection
};

typedef struct {
    FuriHalBleProfileBase base; // must be first
    ShutterKbReport kb_report;
    ShutterConsumerReport consumer_report;
    BleServiceBattery* battery_svc;
    BleServiceDevInfo* dev_info_svc;
    BleServiceHid* hid_svc;
} ShutterProfile;

_Static_assert(offsetof(ShutterProfile, base) == 0, "Wrong layout");

static FuriHalBleProfileBase* shutter_profile_start(FuriHalBleProfileParams profile_params) {
    UNUSED(profile_params);
    ShutterProfile* profile = malloc(sizeof(ShutterProfile));
    memset(profile, 0, sizeof(ShutterProfile));

    profile->base.config = shutter_profile;

    profile->battery_svc = ble_svc_battery_start(true);
    profile->dev_info_svc = ble_svc_dev_info_start();
    profile->hid_svc = ble_svc_hid_start();

    ble_svc_hid_update_report_map(
        profile->hid_svc, shutter_report_map, sizeof(shutter_report_map));

    // HID Information: HID spec 1.01, no country code, remote wake + normally connectable
    uint8_t hid_info[4] = {0x01, 0x01, 0x00, 0x03};
    ble_svc_hid_update_info(profile->hid_svc, hid_info);

    return &profile->base;
}

static void shutter_profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base);
    furi_check(base->config == shutter_profile);
    ShutterProfile* profile = (ShutterProfile*)base;

    ble_svc_battery_stop(profile->battery_svc);
    ble_svc_dev_info_stop(profile->dev_info_svc);
    ble_svc_hid_stop(profile->hid_svc);
    // The profile struct itself is freed by the Bluetooth service
}

static void shutter_profile_get_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    furi_check(config);
    memset(config, 0, sizeof(GapConfig));

    config->adv_service.UUID_Type = SHUTTER_UUID_TYPE_16;
    config->adv_service.Service_UUID_16 = SHUTTER_HID_SERVICE_UUID;
    config->appearance_char = SHUTTER_APPEARANCE_KEYBOARD;

    // THE important part: remember the paired device (bonding), but pair without PIN
    config->bonding_mode = true;
    config->pairing_method = GapPairingNone;

    // Same connection parameters as the stock HID profile (7.5 ms .. 45 ms)
    config->conn_param.conn_int_min = 0x06;
    config->conn_param.conn_int_max = 0x24;
    config->conn_param.slave_latency = 0;
    config->conn_param.supervisor_timeout = 0;

    // Own MAC address, so the device sees a fresh remote (not the old "Control ..." one)
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->mac_address[2] += 2;
    config->mac_address[0] ^= 0x34;
    config->mac_address[1] ^= 0x58;

    // First byte = advertising type marker (copied from the firmware's own name)
    config->adv_name[0] = furi_hal_version_get_ble_local_device_name_ptr()[0];
    snprintf(config->adv_name + 1, sizeof(config->adv_name) - 1, "%s", SHUTTER_DEVICE_NAME);
}

static const FuriHalBleProfileTemplate shutter_profile_callbacks = {
    .start = shutter_profile_start,
    .stop = shutter_profile_stop,
    .get_gap_config = shutter_profile_get_gap_config,
};

const FuriHalBleProfileTemplate* shutter_profile = &shutter_profile_callbacks;

static ShutterProfile* shutter_cast(FuriHalBleProfileBase* base) {
    furi_check(base);
    furi_check(base->config == shutter_profile);
    return (ShutterProfile*)base;
}

static bool shutter_send_kb(ShutterProfile* profile) {
    return ble_svc_hid_update_input_report(
        profile->hid_svc,
        ReportNumberKeyboard,
        (uint8_t*)&profile->kb_report,
        sizeof(ShutterKbReport));
}

static bool shutter_send_consumer(ShutterProfile* profile) {
    return ble_svc_hid_update_input_report(
        profile->hid_svc,
        ReportNumberConsumer,
        (uint8_t*)&profile->consumer_report,
        sizeof(ShutterConsumerReport));
}

bool shutter_profile_key_press(FuriHalBleProfileBase* base, uint8_t keycode) {
    ShutterProfile* profile = shutter_cast(base);
    memset(&profile->kb_report, 0, sizeof(ShutterKbReport));
    profile->kb_report.key[0] = keycode;
    return shutter_send_kb(profile);
}

bool shutter_profile_key_release(FuriHalBleProfileBase* base) {
    ShutterProfile* profile = shutter_cast(base);
    memset(&profile->kb_report, 0, sizeof(ShutterKbReport));
    return shutter_send_kb(profile);
}

bool shutter_profile_consumer_press(FuriHalBleProfileBase* base, uint16_t usage) {
    ShutterProfile* profile = shutter_cast(base);
    profile->consumer_report.usage = usage;
    return shutter_send_consumer(profile);
}

bool shutter_profile_consumer_release(FuriHalBleProfileBase* base) {
    ShutterProfile* profile = shutter_cast(base);
    profile->consumer_report.usage = 0;
    return shutter_send_consumer(profile);
}

void shutter_profile_release_all(FuriHalBleProfileBase* base) {
    shutter_profile_key_release(base);
    shutter_profile_consumer_release(base);
}
