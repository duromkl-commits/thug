/* THUG desktop -- the one zlib function the Vita backend uses (texsum
 * diagnostic). Implemented in desktop/src/desktop_runtime.cpp. */
#ifndef THUG_DESKTOP_ZLIB_H
#define THUG_DESKTOP_ZLIB_H
#ifdef __cplusplus
extern "C" {
#endif
unsigned long crc32( unsigned long crc, const unsigned char *buf, unsigned int len );
#ifdef __cplusplus
}
#endif
#endif
