/* keyboard.h — keyboard input interface. */

#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Initialize the driver.  Currently a no-op (we're in polled mode), but
 * kept so callers can init uniformly and a future IRQ-driven version has
 * somewhere to enable the IRQ line. */
void keyboard_init(void);

/* Block (by polling) until a printable or control character is produced.
 * Returns ASCII: letters, digits, punctuation, space, '\n', '\b', '\t',
 * and 27 (ESC).  Keys with no ASCII mapping (Ctrl, Alt, F-keys, arrows,
 * extended 0xE0-prefixed codes) are silently dropped and polling continues. */
char keyboard_getchar(void);

#endif
