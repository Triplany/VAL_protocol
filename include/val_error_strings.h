#ifndef VAL_ERROR_STRINGS_H
#define VAL_ERROR_STRINGS_H

#include "val_errors.h"
#include <stddef.h>
#include <stdint.h>

#ifdef VAL_HOST_UTILITIES

typedef struct
{
    const char *category;
    const char *description;
    const char *suggestion;
} val_error_info_t;

const char *val_status_to_string(val_status_t status);
const char *val_error_detail_to_string(uint32_t detail_mask);
void val_format_error_report(val_status_t code, uint32_t detail, char *buffer, size_t buffer_size);
int val_analyze_error(val_status_t code, uint32_t detail, val_error_info_t *info);

#endif // VAL_HOST_UTILITIES

#endif // VAL_ERROR_STRINGS_H