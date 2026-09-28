/* Stand-in for the IDF-generated sdkconfig.h (force-included by the host
 * Makefile, like the real build does). Host tests behave like a single-port
 * target; the S3 USB path is covered by CI firmware builds, not here. */
#pragma once
#define CONFIG_IDF_TARGET "esp32c6"
