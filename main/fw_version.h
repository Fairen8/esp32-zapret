// SPDX-License-Identifier: MIT
#pragma once

/* FW_VERSION is injected by the build from the repo-root VERSION file
 * (see main/CMakeLists.txt); "dev" is the fallback for host builds. */
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

#define FW_USER_AGENT "esp32-zapret/" FW_VERSION
