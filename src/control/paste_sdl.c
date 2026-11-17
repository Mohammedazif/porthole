#include "control/paste.h"

#include <string.h>

#include <SDL.h>

int ph_paste_clipboard(char *buf, size_t len)
{
    char *text;
    size_t n;

    if (!buf || len == 0)
        return -1;
    buf[0] = '\0';
    text = SDL_GetClipboardText();
    if (!text)
        return -1;
    n = strlen(text);
    if (n >= len || n > 0x7fffffffu) {
        SDL_free(text);
        return -1;
    }
    memcpy(buf, text, n + 1);
    SDL_free(text);
    return (int)n;
}
