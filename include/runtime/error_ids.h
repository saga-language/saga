/* Copyright 2026 Rob Thornton
 * SPDX-License-Identifier: MIT
 *
 * Reserved type_ids for built-in errors. Shared between the C runtime
 * (runtime.c) and the compiler's codegen so both stamp the same identity.
 *
 * User-declared errors derive their id from an FNV-1a hash of the mangled
 * type name; these small reserved values sit below that range.
 */
#ifndef SAGA_RUNTIME_ERROR_IDS_H
#define SAGA_RUNTIME_ERROR_IDS_H

#include <stdint.h>

#define SAGA_ERR_ID_MISSING ((int64_t)1)
#define SAGA_ERR_ID_TRAPPED ((int64_t)2)

#endif /* SAGA_RUNTIME_ERROR_IDS_H */
