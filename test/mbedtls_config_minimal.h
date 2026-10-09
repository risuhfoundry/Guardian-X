#ifndef MBEDTLS_CONFIG_MINIMAL_H
#define MBEDTLS_CONFIG_MINIMAL_H

/*
 * Minimal mbedTLS 2.28 configuration for the host-side Guardian X tests.
 * Only the modules needed for PBKDF2-HMAC-SHA256 are enabled:
 * platform wrappers, the message-digest layer, SHA-256, and PKCS#5.
 * test/native_test_build.sh installs this file as the build's
 * mbedtls/mbedtls_config.h inside the downloaded mbedTLS sources.
 */

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_PKCS5_C

#include "mbedtls/check_config.h"

#endif /* MBEDTLS_CONFIG_MINIMAL_H */
