// SPDX-License-Identifier: MIT
#pragma once

/* First-boot provisioning. Never returns: called instead of the normal
 * startup when the device has no saved configuration. Starts the setup
 * access point, the web UI (when enabled) and a serial console with
 * setwifi/settoken/setadmin/setmac/setwebpass commands. */
void setup_mode_run(void);
