/*
 * wifi_manager.h — Soft-AP setup.
 *
 * Brings up a 2.4 GHz access point named MOUSEUM_AP_SSID (default: "mouseum").
 * The HTTP server listens on the AP gateway address (192.168.4.1).
 *
 * STA mode is not implemented in this initial cut — it is called out as a
 * follow-up in section 12 of the design doc.
 */
#pragma once

void wifi_manager_start_softap(void);
