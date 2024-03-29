/*
* Copyright 2016-2023, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*/

/** @file
 *
 * This demo application shows an implementation of a low_power_led system.
 * The app uses On/Off control because PWM is not supported in the low power states.
 * The app is based on the snip/mesh/mesh_power_onoff_server sample which implements generic LE Mesh Power Onoff Server model.
 *
 * Features demonstrated
 * - showcase a LPN + Server as well as a Friend node implementation in conjunction with the Proxy/Relay + Server (3x Lightbulb).
 *
 * See chip specific readme.txt for more information about the Bluetooth SDK.
 *
 * To demonstrate the app, work through the following steps.
 * 1. Build and download the application (to the WICED board)
 *      1) For low_power_led lpn system node, add LOW_POWER_NODE=1 in Make Target
 *      2) For lighting element, add LOW_POWER_NODE=0 in Make Target
 * 2. Use Android MeshController or Windows Mesh Client to provision all boards.
 * 3. After successful provisioning, the low_power_led lpn node will choose one lighting node as its friend automatically.
 *    User can use the Android MeshController/ Windows Mesh Client to light on/off all the nodes:
 *    1) For lighting nodes, on/off them directly and they will reflect the result immediately;
 *    2) For low_power_led nodes, it can't receive on/off command directly, it will get command from friend node.
 *       So it needs to create friendship before sleep;
 *    3) When low_power_led node is sleeping, send on/off command to it using Android MeshController or Windows Mesh Client,
 *       the lighting nodes will relay(if distance is far) and cache it(friend node do this). When the low_power_led node
 *       wakes up from sleep, it will poll the friend and then friend node will send the cached on/off command to it.
 *    4) When low_power_led node is in sleep mode, it can maintain its gpio state. You can choose maintain or close it.
 *       When low_power_led node comes back from sleep mode, it can restore the led state.
 */

#include "wiced_bt_ble.h"
#include "wiced_bt_gatt.h"
#include "wiced_bt_mesh_models.h"
#include "wiced_bt_trace.h"
#include "wiced_bt_mesh_app.h"
#include "wiced_sleep.h"
#include "wiced_platform.h"
#include "wiced_timer.h"
#include "led_control.h"
#include "wiced_hal_nvram.h"
#include "wiced_hal_mia.h"
#if defined(NETWORK_FILTER_SERVER_SUPPORTED)
#include "wiced_bt_mesh_mdf.h"
#endif


#ifdef HCI_CONTROL
#include "wiced_transport.h"
#include "hci_control_api.h"
#endif

#include "wiced_bt_cfg.h"
extern wiced_bt_cfg_settings_t wiced_bt_cfg_settings;

/******************************************************
 *          Constants
 ******************************************************/
#define MESH_PID                0x3125
#define MESH_VID                0x0002

#define TRANSITION_INTERVAL     100     // receive status notifications every 100ms during transition to new state

/******************************************************
 *          Structures
 ******************************************************/
typedef struct
{
    uint8_t         present_onoff;
    uint8_t         target_onoff;
#if defined(LOW_POWER_NODE) && (LOW_POWER_NODE == 1)
    wiced_sleep_config_t   lpn_sleep_config;
    wiced_timer_t          lpn_wake_timer;

// Device LPN state
#define MESH_LPN_STATE_NOT_IDLE   0
#define MESH_LPN_STATE_IDLE       1
    uint8_t                lpn_state;    // LPN state: IDLE or NOT_IDLE
#endif
} mesh_low_power_led_t;

/******************************************************
 *          Function Prototypes
 ******************************************************/
static void mesh_app_init(wiced_bool_t is_provisioned);
static void mesh_low_power_led_message_handler(uint8_t element_idx, uint16_t event, void *p_data);
static void mesh_low_power_led_process_status(uint8_t element_idx, wiced_bt_mesh_onoff_status_data_t *p_data);
#if defined(LOW_POWER_NODE) && (LOW_POWER_NODE == 1)
void mesh_low_power_led_lpn_sleep(uint32_t duration);
static uint32_t mesh_low_power_led_sleep_poll(wiced_sleep_poll_type_t type);
static void wakeup_timer_cb(TIMER_PARAM_TYPE arg);

#endif

/******************************************************
 *          Variables Definitions
 ******************************************************/
uint8_t mesh_mfr_name[WICED_BT_MESH_PROPERTY_LEN_DEVICE_MANUFACTURER_NAME]          = { 'C', 'y', 'p', 'r', 'e', 's', 's', 0 };
uint8_t mesh_model_num[WICED_BT_MESH_PROPERTY_LEN_DEVICE_MODEL_NUMBER]              = { '1', '2', '3', '4', 0, 0, 0, 0 };
uint8_t mesh_prop_fw_version[WICED_BT_MESH_PROPERTY_LEN_DEVICE_FIRMWARE_REVISION]   = { '0', '6', '.', '0', '2', '.', '0', '5' }; // this is overwritten during init
uint8_t mesh_system_id[8]                                                           = { 0xbb, 0xb8, 0xa1, 0x80, 0x5f, 0x9f, 0x91, 0x71 };
mesh_low_power_led_t app_state = { 0 };

wiced_bt_mesh_core_config_model_t   mesh_element1_models[] =
{
    WICED_BT_MESH_DEVICE,
#ifdef NETWORK_FILTER_SERVER_SUPPORTED
        WICED_BT_MESH_NETWORK_FILTER_SERVER,
#endif
    WICED_BT_MESH_MODEL_USER_PROPERTY_SERVER,
    WICED_BT_MESH_MODEL_POWER_ONOFF_SERVER,
};

wiced_bt_mesh_core_config_property_t mesh_element1_properties[] =
{
    {
        .id          = WICED_BT_MESH_PROPERTY_DEVICE_FIRMWARE_REVISION,
        .type        = WICED_BT_MESH_PROPERTY_TYPE_USER,
        .user_access = WICED_BT_MESH_PROPERTY_ID_READABLE,
        .max_len     = WICED_BT_MESH_PROPERTY_LEN_DEVICE_FIRMWARE_REVISION,
        .value       = mesh_prop_fw_version
    },
};
#define MESH_APP_NUM_PROPERTIES (sizeof(mesh_element1_properties) / sizeof(wiced_bt_mesh_core_config_property_t))


#define MESH_LOW_POWER_LED_ELEMENT_INDEX   0

wiced_bt_mesh_core_config_element_t mesh_elements[] =
{
    {
        .location = MESH_ELEM_LOC_MAIN,                                 // location description as defined in the GATT Bluetooth Namespace Descriptors section of the Bluetooth SIG Assigned Numbers
        .default_transition_time = MESH_DEFAULT_TRANSITION_TIME_IN_MS,  // Default transition time for models of the element in milliseconds
        .onpowerup_state = WICED_BT_MESH_ON_POWER_UP_STATE_RESTORE,     // Default element behavior on power up
        .default_level = 0,                                             // Default value of the variable controlled on this element (for example power, lightness, temperature, hue...)
        .range_min = 1,                                                 // Minimum value of the variable controlled on this element (for example power, lightness, temperature, hue...)
        .range_max = 0xffff,                                            // Maximum value of the variable controlled on this element (for example power, lightness, temperature, hue...)
        .move_rollover = 0,                                             // If true when level gets to range_max during move operation, it switches to min, otherwise move stops.
        .properties_num = MESH_APP_NUM_PROPERTIES,                      // Number of properties in the array models
        .properties = mesh_element1_properties,                         // Array of properties in the element.
        .sensors_num = 0,                                               // Number of sensors in the sensor array
        .sensors = NULL,                                                // Array of sensors of that element
        .models_num = (sizeof(mesh_element1_models) / sizeof(wiced_bt_mesh_core_config_model_t)),    // Number of models in the array models
        .models = mesh_element1_models,                                 // Array of models located in that element. Model data is defined by structure wiced_bt_mesh_core_config_model_t
    },
};

wiced_bt_mesh_core_config_t  mesh_config =
{
    .company_id         = MESH_COMPANY_ID_CYPRESS,                  // Company identifier assigned by the Bluetooth SIG
    .product_id         = MESH_PID,                                 // Vendor-assigned product identifier
    .vendor_id          = MESH_VID,                                 // Vendor-assigned product version identifier
#if defined(LOW_POWER_NODE) && (LOW_POWER_NODE == 1)
    .features           = WICED_BT_MESH_CORE_FEATURE_BIT_LOW_POWER, // A bit field indicating the device features. In Low Power mode no Relay, no Proxy and no Friend
    .friend_cfg         =                                           // Empty Configuration of the Friend Feature
    {
        .receive_window = 0,                                        // Receive Window value in milliseconds supported by the Friend node.
        .cache_buf_len  = 0,                                        // Length of the buffer for the cache
        .max_lpn_num    = 0                                         // Max number of Low Power Nodes with established friendship. Must be > 0 if Friend feature is supported.
    },
    .low_power          =                                           // Configuration of the Low Power Feature
    {
        .rssi_factor           = 2,                                 // contribution of the RSSI measured by the Friend node used in Friend Offer Delay calculations.
        .receive_window_factor = 2,                                 // contribution of the supported Receive Window used in Friend Offer Delay calculations.
        .min_cache_size_log    = 3,                                 // minimum number of messages that the Friend node can store in its Friend Cache.
        .receive_delay         = 100,                               // Receive delay in 1 ms units to be requested by the Low Power node.
        .poll_timeout          = 200                                // Poll timeout in 100ms units to be requested by the Low Power node.
    },
#else
    .features = WICED_BT_MESH_CORE_FEATURE_BIT_FRIEND | WICED_BT_MESH_CORE_FEATURE_BIT_RELAY | WICED_BT_MESH_CORE_FEATURE_BIT_GATT_PROXY_SERVER,   // Supports Friend, Relay and GATT Proxy
    .friend_cfg         =                                           // Configuration of the Friend Feature(Receive Window in Ms, messages cache)
    {
        .receive_window        = 20,
        .cache_buf_len         = 300,                               // Length of the buffer for the cache
        .max_lpn_num           = 4                                  // Max number of Low Power Nodes with established friendship. Must be > 0 if Friend feature is supported.
    },
    .low_power          =                                           // Configuration of the Low Power Feature
    {
        .rssi_factor           = 0,                                 // contribution of the RSSI measured by the Friend node used in Friend Offer Delay calculations.
        .receive_window_factor = 0,                                 // contribution of the supported Receive Window used in Friend Offer Delay calculations.
        .min_cache_size_log    = 0,                                 // minimum number of messages that the Friend node can store in its Friend Cache.
        .receive_delay         = 0,                                 // Receive delay in 1 ms units to be requested by the Low Power node.
        .poll_timeout          = 0                                  // Poll timeout in 100ms units to be requested by the Low Power node.
    },
#endif
    .gatt_client_only          = WICED_FALSE,                       // Can connect to mesh over GATT or ADV
    .elements_num  = (uint8_t)(sizeof(mesh_elements) / sizeof(mesh_elements[0])),   // number of elements on this device
    .elements      = mesh_elements                                  // Array of elements for this device
};

/*
 * Mesh application library will call into application functions if provided by the application.
 */
wiced_bt_mesh_app_func_table_t wiced_bt_mesh_app_func_table =
{
    mesh_app_init,          // application initialization
    NULL,                   // Default SDK platform button processing
    NULL,                   // GATT connection status
    NULL,                   // attention processing
    NULL,                   // notify period set
    NULL,                   // WICED HCI command
#if defined(LOW_POWER_NODE) && (LOW_POWER_NODE == 1)
    mesh_low_power_led_lpn_sleep,// LPN sleep
#else
    NULL,
#endif
    NULL                    // factory reset
};

wiced_bool_t do_not_init_again = WICED_FALSE;

/******************************************************
 *               Function Definitions
 ******************************************************/
void mesh_app_init(wiced_bool_t is_provisioned)
{
#if 0
    // Set Debug trace level for mesh_models_lib and mesh_provisioner_lib
    wiced_bt_mesh_models_set_trace_level(WICED_BT_MESH_CORE_TRACE_INFO);
#endif
#if 0
    // Set Debug trace level for all modules but Info level for CORE_AES_CCM module
    wiced_bt_mesh_core_set_trace_level(WICED_BT_MESH_CORE_TRACE_FID_ALL, WICED_BT_MESH_CORE_TRACE_DEBUG);
    wiced_bt_mesh_core_set_trace_level(WICED_BT_MESH_CORE_TRACE_FID_CORE_AES_CCM, WICED_BT_MESH_CORE_TRACE_INFO);
#endif
    wiced_result_t  result;

    // This means that device came out of HID off mode and it is not a power cycle
    if(wiced_hal_mia_is_reset_reason_por())
    {
        WICED_BT_TRACE("start reason: reset\n");
    }
    else
    {
#if CYW20819A1
        if(wiced_hal_mia_is_reset_reason_hid_timeout())
        {
            WICED_BT_TRACE("Wake from HID off: timed wake\n");
        }
        else
#endif
        {
            // Check if we wake up by GPIO
            WICED_BT_TRACE("Wake from HID off, interrupt:%d\n", wiced_hal_gpio_get_pin_interrupt_status(WICED_GPIO_PIN_BUTTON));
        }
    }

#if defined(LOW_POWER_NODE) && (LOW_POWER_NODE == 1)
    wiced_bt_cfg_settings.device_name = (uint8_t *)"Low Power LED";
#else
    wiced_bt_cfg_settings.device_name = (uint8_t *)"On/Off LED";
#endif
    wiced_bt_cfg_settings.gatt_cfg.appearance = APPEARANCE_GENERIC_TAG;

    // Adv Data is fixed. Spec allows to put URI, Name, Appearance and Tx Power in the Scan Response Data.
    if (!is_provisioned)
    {
        wiced_bt_ble_advert_elem_t  adv_elem[3];
        uint8_t                     buf[2];
        uint8_t                     num_elem = 0;

        adv_elem[num_elem].advert_type = BTM_BLE_ADVERT_TYPE_NAME_COMPLETE;
        adv_elem[num_elem].len         = (uint16_t)strlen((const char*)wiced_bt_cfg_settings.device_name);
        adv_elem[num_elem].p_data      = wiced_bt_cfg_settings.device_name;
        num_elem++;

        adv_elem[num_elem].advert_type = BTM_BLE_ADVERT_TYPE_APPEARANCE;
        adv_elem[num_elem].len         = 2;
        buf[0]                         = (uint8_t)wiced_bt_cfg_settings.gatt_cfg.appearance;
        buf[1]                         = (uint8_t)(wiced_bt_cfg_settings.gatt_cfg.appearance >> 8);
        adv_elem[num_elem].p_data      = buf;
        num_elem++;

        wiced_bt_mesh_set_raw_scan_response_data(num_elem, adv_elem);
    }

    mesh_prop_fw_version[0] = 0x30 + (WICED_SDK_MAJOR_VER / 10);
    mesh_prop_fw_version[1] = 0x30 + (WICED_SDK_MAJOR_VER % 10);
    mesh_prop_fw_version[2] = 0x30 + (WICED_SDK_MINOR_VER / 10);
    mesh_prop_fw_version[3] = 0x30 + (WICED_SDK_MINOR_VER % 10);
    mesh_prop_fw_version[4] = 0x30 + (WICED_SDK_REV_NUMBER / 10);
    mesh_prop_fw_version[5] = 0x30 + (WICED_SDK_REV_NUMBER % 10);
    // convert 12 bits of BUILD_NUMMBER to two base64 characters big endian
    mesh_prop_fw_version[6] = wiced_bt_mesh_base64_encode_6bits((uint8_t)(WICED_SDK_BUILD_NUMBER >> 6) & 0x3f);
    mesh_prop_fw_version[7] = wiced_bt_mesh_base64_encode_6bits((uint8_t)WICED_SDK_BUILD_NUMBER & 0x3f);

    led_control_init(LED_CONTROL_TYPE_ONOFF);

#ifdef NETWORK_FILTER_SERVER_SUPPORTED
    if (is_provisioned)
        wiced_bt_mesh_network_filter_init();
#endif

    wiced_bt_mesh_model_power_onoff_server_init(MESH_LOW_POWER_LED_ELEMENT_INDEX, mesh_low_power_led_message_handler, TRANSITION_INTERVAL, is_provisioned);

#if defined(LOW_POWER_NODE) && (LOW_POWER_NODE == 1)
    if (!do_not_init_again)
    {
        WICED_BT_TRACE("Init once \n");

        // Configure to sleep as the device is idle now
        app_state.lpn_sleep_config.sleep_mode = WICED_SLEEP_MODE_NO_TRANSPORT;
        app_state.lpn_sleep_config.device_wake_mode = WICED_GPIO_BUTTON_WAKE_MODE;
        app_state.lpn_sleep_config.device_wake_source = WICED_SLEEP_WAKE_SOURCE_GPIO;
        app_state.lpn_sleep_config.device_wake_gpio_num = WICED_GPIO_PIN_BUTTON;
        app_state.lpn_sleep_config.host_wake_mode = WICED_SLEEP_WAKE_ACTIVE_HIGH;
        app_state.lpn_sleep_config.sleep_permit_handler = mesh_low_power_led_sleep_poll;
#if defined(CYW20819A1) || defined(CYW20820A1)
        app_state.lpn_sleep_config.post_sleep_cback_handler = NULL;
#endif

        if (WICED_BT_SUCCESS != wiced_sleep_configure(&app_state.lpn_sleep_config))
        {
            WICED_BT_TRACE("Sleep Configure failed\r\n");
        }

        wiced_init_timer(&app_state.lpn_wake_timer, wakeup_timer_cb, 0, WICED_MILLI_SECONDS_TIMER);

        do_not_init_again = WICED_TRUE;
    }
#endif
}

/*
 * Process event received from the models library.
 */
void mesh_low_power_led_message_handler(uint8_t element_idx, uint16_t event, void *p_data)
{
    switch (event)
    {
    case WICED_BT_MESH_ONOFF_STATUS:
        mesh_low_power_led_process_status(element_idx, (wiced_bt_mesh_onoff_status_data_t *)p_data);
        break;

    default:
        WICED_BT_TRACE("unknown\n");
    }
}

/*
 * This function is called when command to change state is received over mesh.
 */
void mesh_low_power_led_process_status(uint8_t element_idx, wiced_bt_mesh_onoff_status_data_t *p_status)
{
    led_control_set_onoff(p_status->present_onoff);
}

/*
 * Put the board into sleep mode.
 */
#if defined(LOW_POWER_NODE) && (LOW_POWER_NODE == 1)
void mesh_low_power_led_lpn_sleep(uint32_t max_sleep_duration)
{
#if defined(CYW20835B1)
	// Enter SDS (Shut Down Sleep) will save more power than PMU Sleep. But it's up to your design.
	if(max_sleep_duration != WICED_SLEEP_MAX_TIME_TO_SLEEP)
	{
		wiced_stop_timer(&app_state.lpn_wake_timer);
		wiced_start_timer(&app_state.lpn_wake_timer, max_sleep_duration);
	}
	WICED_BT_TRACE("Get ready to go into SDS, duration=%d\n\r", max_sleep_duration);
	app_state.lpn_state = MESH_LPN_STATE_IDLE;
#else
	// Generally speaking, if sleep timer bigger than 2mins, then hid-off will save more power. But it's up to your design.
    if (max_sleep_duration < 120000)//2mins
    {
        wiced_stop_timer(&app_state.lpn_wake_timer);
        wiced_start_timer(&app_state.lpn_wake_timer, max_sleep_duration);
        WICED_BT_TRACE("Get ready to go into ePDS sleep, duration=%d\n\r", max_sleep_duration);
        app_state.lpn_state = MESH_LPN_STATE_IDLE;
    }
    else
    {
        WICED_BT_TRACE("Entering HID-OFF for max_sleep_duration: %d\r\n", max_sleep_duration);
        if (WICED_SUCCESS != wiced_sleep_enter_hid_off(max_sleep_duration, WICED_GPIO_PIN_BUTTON, 1))
        {
            WICED_BT_TRACE("Entering HID-Off failed\n\r");
        }
    }
#endif
}

/*
 * wakeup timer callback.
 * ePDS is default sleep mode(current is about 10uA).
 */
static void wakeup_timer_cb(TIMER_PARAM_TYPE arg)
{
    WICED_BT_TRACE("ePDS wake up!!!\n");
    app_state.lpn_state = MESH_LPN_STATE_NOT_IDLE;
    wiced_stop_timer(&app_state.lpn_wake_timer);
}


/*
 * Sleep permission polling time to be used by firmware
 */
static uint32_t mesh_low_power_led_sleep_poll(wiced_sleep_poll_type_t type)
{
    uint32_t ret = WICED_SLEEP_NOT_ALLOWED;

    switch (type)
    {
    case WICED_SLEEP_POLL_TIME_TO_SLEEP:
        if (app_state.lpn_state == MESH_LPN_STATE_NOT_IDLE)
        {
            WICED_BT_TRACE("!");
            ret = WICED_SLEEP_NOT_ALLOWED;
        }
        else
        {
            WICED_BT_TRACE("@\n");
            ret = WICED_SLEEP_MAX_TIME_TO_SLEEP;
        }
        break;
    case WICED_SLEEP_POLL_SLEEP_PERMISSION:
        if (app_state.lpn_state == MESH_LPN_STATE_IDLE)
        {
            WICED_BT_TRACE("#\n");
#if defined(CYW20835B1)
            ret = WICED_SLEEP_ALLOWED_WITH_SHUTDOWN;
#else
            ret = WICED_SLEEP_ALLOWED_WITHOUT_SHUTDOWN;
#endif
        }

        break;
    }
    return ret;
}
#endif
