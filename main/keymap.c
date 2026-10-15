/* Character -> HID usage for the host's keyboard layout. */

#include "espkb.h"

#define S(k) (0x80 | (k)) /* shifted */

static uint8_t lookup_us(uint32_t c)
{
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return S(0x04 + (c - 'A'));
    if (c >= '1' && c <= '9') return 0x1E + (c - '1');

    switch (c) {
    case '0': return 0x27;
    case '\n':
    case '\r': return 0x28;
    case '\b': return 0x2A;
    case '\t': return 0x2B;
    case ' ': return 0x2C;
    case '!': return S(0x1E);
    case '@': return S(0x1F);
    case '#': return S(0x20);
    case '$': return S(0x21);
    case '%': return S(0x22);
    case '^': return S(0x23);
    case '&': return S(0x24);
    case '*': return S(0x25);
    case '(': return S(0x26);
    case ')': return S(0x27);
    case '-': return 0x2D;
    case '_': return S(0x2D);
    case '=': return 0x2E;
    case '+': return S(0x2E);
    case '[': return 0x2F;
    case '{': return S(0x2F);
    case ']': return 0x30;
    case '}': return S(0x30);
    case '\\': return 0x31;
    case '|': return S(0x31);
    case ';': return 0x33;
    case ':': return S(0x33);
    case '\'': return 0x34;
    case '"': return S(0x34);
    case '`': return 0x35;
    case '~': return S(0x35);
    case ',': return 0x36;
    case '<': return S(0x36);
    case '.': return 0x37;
    case '>': return S(0x37);
    case '/': return 0x38;
    case '?': return S(0x38);
    default: return 0;
    }
}

#if CONFIG_ESPKB_LAYOUT_UK
static uint8_t lookup_uk(uint32_t c)
{
    switch (c) {
    case '"': return S(0x1F);
    case '@': return S(0x34);
    case '#': return 0x32;
    case '~': return S(0x32);
    case '\\': return 0x64;
    case '|': return S(0x64);
    case 0xA3: return S(0x20); /* £ */
    case 0xAC: return S(0x35); /* ¬ */
    default: return lookup_us(c);
    }
}
#endif

bool keymap_lookup(uint32_t codepoint, uint8_t *mods, uint8_t *key)
{
#if CONFIG_ESPKB_LAYOUT_UK
    uint8_t v = lookup_uk(codepoint);
#else
    uint8_t v = lookup_us(codepoint);
#endif
    if (!v) return false;
    *mods = (v & 0x80) ? KM_SHIFT : 0;
    *key = v & 0x7F;
    return true;
}
