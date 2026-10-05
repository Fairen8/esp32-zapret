// SPDX-License-Identifier: MIT
#pragma once

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  else
#    include "secrets_example.h"
#    warning "main/secrets.h not found, using secrets_example.h"
#  endif
#else
#  include "secrets.h"
#endif

/* Backward compatibility with older secrets.h */
#ifndef CFG_TG_API_IPS
#define CFG_TG_API_IPS ""
#endif
