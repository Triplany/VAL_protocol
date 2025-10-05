#ifndef VAL_BYTE_ORDER_H
#define VAL_BYTE_ORDER_H

#include <stdint.h>

// Cross-compiler force-inline hint
#ifndef VAL_FORCE_INLINE
    #if defined(_MSC_VER)
        #define VAL_FORCE_INLINE __forceinline
    #elif defined(__GNUC__) || defined(__clang__)
        #define VAL_FORCE_INLINE inline __attribute__((always_inline))
    #else
        #define VAL_FORCE_INLINE inline
    #endif
#endif

// Test override hooks: allow forcing endian for a single translation unit
#if defined(VAL_FORCE_LITTLE_ENDIAN) || defined(VAL_FORCE_BIG_ENDIAN)
    #undef VAL_LITTLE_ENDIAN
    #undef VAL_BIG_ENDIAN
    #if defined(VAL_FORCE_LITTLE_ENDIAN)
        #define VAL_LITTLE_ENDIAN 1
        #define VAL_BIG_ENDIAN 0
    #else
        #define VAL_LITTLE_ENDIAN 0
        #define VAL_BIG_ENDIAN 1
    #endif
#endif

// Detect endianness at compile time
#if !defined(VAL_LITTLE_ENDIAN) && !defined(VAL_BIG_ENDIAN)
    #if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
        #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            #define VAL_LITTLE_ENDIAN 1
            #define VAL_BIG_ENDIAN 0
        #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            #define VAL_LITTLE_ENDIAN 0
            #define VAL_BIG_ENDIAN 1
        #endif
    #elif defined(_WIN32) || defined(__i386__) || defined(__x86_64__) || defined(__aarch64__)
        // Windows, x86, x64, ARM64 are little-endian
        #define VAL_LITTLE_ENDIAN 1
        #define VAL_BIG_ENDIAN 0
    #endif
#endif

// If only one is defined, define the complement to avoid inconsistent state without redefining existing macros
#if defined(VAL_LITTLE_ENDIAN) && !defined(VAL_BIG_ENDIAN)
    #if VAL_LITTLE_ENDIAN
        #define VAL_BIG_ENDIAN 0
    #else
        #define VAL_BIG_ENDIAN 1
    #endif
#endif
#if !defined(VAL_LITTLE_ENDIAN) && defined(VAL_BIG_ENDIAN)
    #if VAL_BIG_ENDIAN
        #define VAL_LITTLE_ENDIAN 0
    #else
        #define VAL_LITTLE_ENDIAN 1
    #endif
#endif

#ifndef VAL_LITTLE_ENDIAN
#define VAL_LITTLE_ENDIAN 0
#endif

#ifndef VAL_BIG_ENDIAN
#define VAL_BIG_ENDIAN 0
#endif

#if !defined(VAL_RUNTIME_ENDIAN_CHECK)
    #if !VAL_LITTLE_ENDIAN && !VAL_BIG_ENDIAN
        #define VAL_RUNTIME_ENDIAN_CHECK 1
    #else
        #define VAL_RUNTIME_ENDIAN_CHECK 0
    #endif
#endif

#if VAL_RUNTIME_ENDIAN_CHECK
static VAL_FORCE_INLINE int val_runtime_is_little_endian(void)
{
    const uint16_t x = 0x00FFu;
    return (*((const uint8_t *)&x) == 0xFFu);
}
#else
static VAL_FORCE_INLINE int val_runtime_is_little_endian(void)
{
#if VAL_LITTLE_ENDIAN
    return 1;
#else
    return 0;
#endif
}
#endif

static VAL_FORCE_INLINE uint16_t val_bswap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static VAL_FORCE_INLINE uint32_t val_bswap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

static VAL_FORCE_INLINE uint64_t val_bswap64(uint64_t v)
{
    return ((uint64_t)val_bswap32((uint32_t)(v & 0xFFFFFFFFu)) << 32) |
           (uint64_t)val_bswap32((uint32_t)(v >> 32));
}

static VAL_FORCE_INLINE uint16_t val_htole16(uint16_t v)
{
#if VAL_LITTLE_ENDIAN
    return v;
#elif VAL_BIG_ENDIAN
    return val_bswap16(v);
#else
    return val_runtime_is_little_endian() ? v : val_bswap16(v);
#endif
}

static VAL_FORCE_INLINE uint32_t val_htole32(uint32_t v)
{
#if VAL_LITTLE_ENDIAN
    return v;
#elif VAL_BIG_ENDIAN
    return val_bswap32(v);
#else
    return val_runtime_is_little_endian() ? v : val_bswap32(v);
#endif
}

static VAL_FORCE_INLINE uint64_t val_htole64(uint64_t v)
{
#if VAL_LITTLE_ENDIAN
    return v;
#elif VAL_BIG_ENDIAN
    return val_bswap64(v);
#else
    return val_runtime_is_little_endian() ? v : val_bswap64(v);
#endif
}

static VAL_FORCE_INLINE uint16_t val_letoh16(uint16_t v)
{
    return val_htole16(v);
}

static VAL_FORCE_INLINE uint32_t val_letoh32(uint32_t v)
{
    return val_htole32(v);
}

static VAL_FORCE_INLINE uint64_t val_letoh64(uint64_t v)
{
    return val_htole64(v);
}

#if VAL_LITTLE_ENDIAN
    #define VAL_PUT_LE16(buf, val) (*((uint16_t *)(void *)(buf)) = (uint16_t)(val))
    #define VAL_PUT_LE32(buf, val) (*((uint32_t *)(void *)(buf)) = (uint32_t)(val))
    #define VAL_PUT_LE64(buf, val) (*((uint64_t *)(void *)(buf)) = (uint64_t)(val))

    #define VAL_GET_LE16(buf) (*((const uint16_t *)(const void *)(buf)))
    #define VAL_GET_LE32(buf) (*((const uint32_t *)(const void *)(buf)))
    #define VAL_GET_LE64(buf) (*((const uint64_t *)(const void *)(buf)))
#elif VAL_BIG_ENDIAN
    #define VAL_PUT_LE16(buf, val) do { \
        uint16_t __v = (uint16_t)(val); \
        (buf)[0] = (uint8_t)(__v & 0xFFu); \
        (buf)[1] = (uint8_t)((__v >> 8) & 0xFFu); \
    } while (0)

    #define VAL_PUT_LE32(buf, val) do { \
        uint32_t __v = (uint32_t)(val); \
        (buf)[0] = (uint8_t)(__v & 0xFFu); \
        (buf)[1] = (uint8_t)((__v >> 8) & 0xFFu); \
        (buf)[2] = (uint8_t)((__v >> 16) & 0xFFu); \
        (buf)[3] = (uint8_t)((__v >> 24) & 0xFFu); \
    } while (0)

    #define VAL_PUT_LE64(buf, val) do { \
        uint64_t __v = (uint64_t)(val); \
        (buf)[0] = (uint8_t)(__v & 0xFFu); \
        (buf)[1] = (uint8_t)((__v >> 8) & 0xFFu); \
        (buf)[2] = (uint8_t)((__v >> 16) & 0xFFu); \
        (buf)[3] = (uint8_t)((__v >> 24) & 0xFFu); \
        (buf)[4] = (uint8_t)((__v >> 32) & 0xFFu); \
        (buf)[5] = (uint8_t)((__v >> 40) & 0xFFu); \
        (buf)[6] = (uint8_t)((__v >> 48) & 0xFFu); \
        (buf)[7] = (uint8_t)((__v >> 56) & 0xFFu); \
    } while (0)

    #define VAL_GET_LE16(buf) \
        ((uint16_t)((buf)[0]) | ((uint16_t)((buf)[1]) << 8))

    #define VAL_GET_LE32(buf) \
        ((uint32_t)(buf)[0] | ((uint32_t)(buf)[1] << 8) | \
         ((uint32_t)(buf)[2] << 16) | ((uint32_t)(buf)[3] << 24))

    #define VAL_GET_LE64(buf) \
        ((uint64_t)(buf)[0] | ((uint64_t)(buf)[1] << 8) | \
         ((uint64_t)(buf)[2] << 16) | ((uint64_t)(buf)[3] << 24) | \
         ((uint64_t)(buf)[4] << 32) | ((uint64_t)(buf)[5] << 40) | \
         ((uint64_t)(buf)[6] << 48) | ((uint64_t)(buf)[7] << 56))
#else
static inline void val_put_le16_runtime(uint8_t *buf, uint16_t val)
{
    if (val_runtime_is_little_endian())
    {
        *((uint16_t *)(void *)buf) = val;
    }
    else
    {
        buf[0] = (uint8_t)(val & 0xFFu);
        buf[1] = (uint8_t)((val >> 8) & 0xFFu);
    }
}

static inline void val_put_le32_runtime(uint8_t *buf, uint32_t val)
{
    if (val_runtime_is_little_endian())
    {
        *((uint32_t *)(void *)buf) = val;
    }
    else
    {
        buf[0] = (uint8_t)(val & 0xFFu);
        buf[1] = (uint8_t)((val >> 8) & 0xFFu);
        buf[2] = (uint8_t)((val >> 16) & 0xFFu);
        buf[3] = (uint8_t)((val >> 24) & 0xFFu);
    }
}

static inline void val_put_le64_runtime(uint8_t *buf, uint64_t val)
{
    if (val_runtime_is_little_endian())
    {
        *((uint64_t *)(void *)buf) = val;
    }
    else
    {
        buf[0] = (uint8_t)(val & 0xFFu);
        buf[1] = (uint8_t)((val >> 8) & 0xFFu);
        buf[2] = (uint8_t)((val >> 16) & 0xFFu);
        buf[3] = (uint8_t)((val >> 24) & 0xFFu);
        buf[4] = (uint8_t)((val >> 32) & 0xFFu);
        buf[5] = (uint8_t)((val >> 40) & 0xFFu);
        buf[6] = (uint8_t)((val >> 48) & 0xFFu);
        buf[7] = (uint8_t)((val >> 56) & 0xFFu);
    }
}

static inline uint16_t val_get_le16_runtime(const uint8_t *buf)
{
    if (val_runtime_is_little_endian())
    {
        return *((const uint16_t *)(const void *)buf);
    }
    return (uint16_t)(buf[0]) | ((uint16_t)(buf[1]) << 8);
}

static inline uint32_t val_get_le32_runtime(const uint8_t *buf)
{
    if (val_runtime_is_little_endian())
    {
        return *((const uint32_t *)(const void *)buf);
    }
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static inline uint64_t val_get_le64_runtime(const uint8_t *buf)
{
    if (val_runtime_is_little_endian())
    {
        return *((const uint64_t *)(const void *)buf);
    }
    return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) |
           ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
           ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
           ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);
}

#define VAL_PUT_LE16(buf, val) val_put_le16_runtime((uint8_t *)(buf), (uint16_t)(val))
#define VAL_PUT_LE32(buf, val) val_put_le32_runtime((uint8_t *)(buf), (uint32_t)(val))
#define VAL_PUT_LE64(buf, val) val_put_le64_runtime((uint8_t *)(buf), (uint64_t)(val))

#define VAL_GET_LE16(buf) val_get_le16_runtime((const uint8_t *)(buf))
#define VAL_GET_LE32(buf) val_get_le32_runtime((const uint8_t *)(buf))
#define VAL_GET_LE64(buf) val_get_le64_runtime((const uint8_t *)(buf))
#endif

#endif // VAL_BYTE_ORDER_H
