# Metadata Validation

The VAL Protocol supports optional metadata validation through a single callback function that gives implementors complete control over file acceptance criteria while keeping the core protocol minimal and MCU‑friendly.

## Configuration

In `val_config_t`:

- `metadata_validation.validator`: A single callback that decides whether to ACCEPT, SKIP, or ABORT each incoming file. If NULL, all files are accepted.
- `metadata_validation.validator_context`: Opaque pointer passed back to the callback.

Helper APIs:
- `val_config_validation_disabled(&cfg);` to disable validation (default).
- `val_config_set_validator(&cfg, my_validator, my_ctx);` to enable with custom logic.

## Callback Signature

```
val_validation_action_t (*val_metadata_validator_t)(
    const val_meta_payload_t *meta,  // File metadata (filename, sender_path, size, CRC)
    const char *target_path,         // Full sanitized path where file will be saved
    void *context                    // User-provided context pointer
);
```

Return values:
- `VAL_VALIDATION_ACCEPT`: Accept file and proceed with transfer
- `VAL_VALIDATION_SKIP`: Skip this file, continue with the next one
- `VAL_VALIDATION_ABORT`: Abort the entire session immediately

## Behavior

- Validation occurs immediately after the receiver processes the SEND_META packet and constructs a sanitized target path.
- When the callback returns SKIP/ABORT, the receiver reuses existing resume responses (SKIP_FILE or ABORT_FILE) to inform the sender. No new wire messages are added.
- When NULL (default), the receiver accepts everything with zero overhead (only a null pointer check).

## Examples

### No Validation (default)

```
val_config_validation_disabled(&cfg); // or leave cfg.metadata_validation.validator == NULL
```

### Simple File Size Limit

```
static val_validation_action_t size_validator(const val_meta_payload_t *meta,
                                              const char *target_path,
                                              void *context) {
    (void)target_path;
    uint64_t max_size = *(uint64_t*)context;
    return (meta->file_size > max_size) ? VAL_VALIDATION_SKIP : VAL_VALIDATION_ACCEPT;
}

uint64_t limit = 10 * 1024 * 1024; // 10MB
val_config_set_validator(&cfg, size_validator, &limit);
```

### File Type Filtering

```
static val_validation_action_t type_validator(const val_meta_payload_t *meta,
                                              const char *target_path,
                                              void *context) {
    (void)target_path; (void)context;
    const char *ext = strrchr(meta->filename, '.');
    if (!ext) return VAL_VALIDATION_SKIP; // no extension
    if (_stricmp(ext, ".txt") == 0 || _stricmp(ext, ".log") == 0 || _stricmp(ext, ".dat") == 0)
        return VAL_VALIDATION_ACCEPT;
    return VAL_VALIDATION_SKIP;
}
```

### Advanced Multiple Criteria

```
typedef struct {
    uint64_t max_file_size;
    const char **blocked_patterns; // NULL-terminated
} validation_cfg_t;

static val_validation_action_t advanced_validator(const val_meta_payload_t *meta,
                                                  const char *target_path,
                                                  void *context) {
    validation_cfg_t *cfg = (validation_cfg_t*)context;
    if (meta->file_size > cfg->max_file_size)
        return VAL_VALIDATION_SKIP;
    for (int i = 0; cfg->blocked_patterns && cfg->blocked_patterns[i]; ++i) {
        if (strstr(meta->filename, cfg->blocked_patterns[i]))
            return VAL_VALIDATION_ABORT; // potential security issue
    }
    (void)target_path; // optionally check disk space, path rules, etc.
    return VAL_VALIDATION_ACCEPT;
}
```

### MCU-Friendly Example

```
static val_validation_action_t mcu_validator(const val_meta_payload_t *meta,
                                             const char *target_path,
                                             void *context) {
    (void)context; (void)target_path;
    if (meta->file_size > 1024 * 1024) // 1MB cap
        return VAL_VALIDATION_SKIP;
    if (strlen(meta->filename) > 32)
        return VAL_VALIDATION_SKIP;
    return VAL_VALIDATION_ACCEPT;
}
```

## TCP Example Integration

`examples/tcp/val_example_receive.c` enables a simple validator by default and supports `--no-validation` to disable it.

```
val_config_set_validator(&cfg, example_validator, NULL);
// ... or
val_config_validation_disabled(&cfg);
```

## Notes

- The constructed `target_path` uses sanitized filename and the receiver’s output directory; never trust `sender_path` for filesystem decisions.
- SKIP uses `VAL_RESUME_ACTION_SKIP_FILE` and ABORT uses `VAL_RESUME_ACTION_ABORT_FILE` over the existing resume response packet.
- Overhead is a single NULL check when no validator is set.
