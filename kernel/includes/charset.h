/* charset.h — ISO-8859-2 (d-os text) <-> UTF-8 (everyone else).  See
 * charset.c.  Both return the bytes written to `out`; an unmappable character
 * becomes '?', and output stops at `cap` on a whole-character boundary. */
#ifndef CHARSET_H
#define CHARSET_H
int charset_latin2_to_utf8(const char* in, int n, char* out, int cap);
int charset_utf8_to_latin2(const char* in, int n, char* out, int cap);
int charset_is_utf8_text(const char* s, int n);   /* valid, with a multi-byte char */
#endif
