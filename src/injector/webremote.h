/* Phone remote hosted by the injector (see webremote.c).
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
int web_start(int port);   /* starts the HTTP server; the page lives in /data/dimod/web */

/* the build id written into the web folder (web/build.txt) and whether it differs from this payload's */
const char *web_build(void);
int web_build_mismatch(void);
