/*
 * http_server.h — REST endpoints listed in section 7.5 of the design doc.
 *
 *   POST /api/v1/move                 {"dx":int,"dy":int}
 *   POST /api/v1/move_human           {"dx":int,"dy":int}
 *   POST /api/v1/click                {"button":"left"|"right"|"middle"}
 *   POST /api/v1/buttons/down         {"mask":int}
 *   POST /api/v1/buttons/up           {"mask":int}
 *   POST /api/v1/buttons/release_all
 *   POST /api/v1/wheel                {"delta":int}
 *   POST /api/v1/autowalk/toggle
 *   GET  /api/v1/status
 *   GET  /api/v1/help
 *   GET  /                            (tiny built-in control panel)
 */
#pragma once

void start_http_server(void);
