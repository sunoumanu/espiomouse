/*
 * wifi_manager.h — Wi-Fi bring-up.
 *
 * `wifi_manager_start()` picks STA or Soft-AP based on board_config.h:
 *   - MOUSEUM_STA_SSID non-empty  => station mode (joins existing network)
 *   - MOUSEUM_STA_SSID empty      => Soft-AP at 192.168.4.1
 *
 * The HTTP server listens on whichever netif is up; no change there.
 */
#pragma once

void wifi_manager_start(void);

/* Lower-level entry points — useful if you want to force a mode at runtime. */
void wifi_manager_start_softap(void);
void wifi_manager_start_sta(void);
