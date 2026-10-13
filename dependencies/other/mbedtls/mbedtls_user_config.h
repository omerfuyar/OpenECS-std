// What the web plugin changes in mbedTLS's default configuration, which includes this file at its end.
#pragma once

// memory comes from the functions that mbedtls_platform_set_calloc_free gives, so the plugin gives SDL's allocator
#define MBEDTLS_PLATFORM_MEMORY
