#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Checks whether the device has been provisioned with:
 * 1. An auth key in the 'auth' NVS namespace.
 * 2. At least one Wi-Fi network in the 'config' NVS namespace.
 */
bool is_device_provisioned(void);

/**
 * Starts the SoftAP, DNS captive portal, and HTTP provisioning server.
 * This function blocks or loops as the provisioning mode runner until reboot.
 */
void start_provisioning_portal(void);

/**
 * Checks whether the device has successfully connected to Wi-Fi at least once.
 * Returns true if connection has been verified (or if flashed via USB).
 */
bool is_wifi_verified(void);

/**
 * Marks the Wi-Fi connection as verified in NVS.
 * Once verified, subsequent Wi-Fi/upload failures will NEVER cause the device to reset.
 */
void mark_wifi_verified(void);

/**
 * Erases unverified Wi-Fi credentials and restarts into Provisioning AP mode.
 * Only called if initial post-provisioning connection fails 3 times.
 */
void reset_to_provisioning_mode(void);

/**
 * Returns configured API Ingest endpoint from NVS if available,
 * otherwise falls back to compile-time API_ENDPOINT.
 */
const char* get_api_endpoint(void);

/**
 * Returns configured Heartbeat endpoint from NVS if available,
 * otherwise falls back to compile-time API_HEARTBEAT_ENDPOINT.
 */
const char* get_heartbeat_endpoint(void);

/**
 * Returns configured Definitions endpoint from NVS if available,
 * otherwise falls back to compile-time API_DEFINITIONS_ENDPOINT.
 */
const char* get_definitions_endpoint(void);

#ifdef __cplusplus
}
#endif

#endif // PROVISIONING_H
