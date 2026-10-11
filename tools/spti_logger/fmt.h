/*
 * fmt.h - platform-neutral JSONL record formatter for the FlexColor
 * DeviceIoControl transport trace.
 *
 * Pure C90, no libc beyond <stddef.h>: this file + fmt.c must compile both
 * as Windows DLL code (ft_hook.dll, i686-w64-mingw32-gcc / cl) and as a
 * native Linux unit test binary.
 *
 * Record contract (must stay compatible with the future trace parser):
 *   one compact JSON object per line, lowercase hex, no spaces, fields:
 *   h, code, in_len, in_head, ok, out_len, out_head
 */
#ifndef FT_FMT_H
#define FT_FMT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of in/out buffer captured in the head fields. Single definition
 * here so the formatter (fmt.c) and the hook (hook.c) cannot drift. */
#define FT_MAX_HEAD 64

/* Writes one JSONL record (no trailing newline) into buf.
 * Returns bytes written (excl. NUL) or -1 if cap is insufficient. */
int ft_fmt_ioctl_record(char *buf, size_t cap,
                        unsigned long h_device,
                        unsigned long io_code,
                        size_t in_len, const unsigned char *in_head, size_t in_head_len,
                        int success, size_t out_len,
                        const unsigned char *out_head, size_t out_head_len);

#ifdef __cplusplus
}
#endif

#endif /* FT_FMT_H */
