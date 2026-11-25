#include "ui/display_scale.h"

#if defined(PH_HAVE_X11) && defined(SDL_VIDEO_DRIVER_X11)
#include <SDL_syswm.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>

static float x11_content_scale(const char *resources)
{
    const char *line = resources;
    while (line && *line) {
        if (strncmp(line, "Xft.dpi:", 8) == 0) {
            const char *value = line + 8;
            while (*value == ' ' || *value == '\t')
                value++;
            char *end;
            float dpi = strtof(value, &end);
            if (end == value || !isfinite(dpi) || dpi <= 0)
                return 1.0f;
            while (*end == ' ' || *end == '\t' || *end == '\r')
                end++;
            if (*end == '\n' || *end == '\0')
                return SDL_clamp(dpi / 96.0f, 0.75f, 4.0f);
            return 1.0f;
        }
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return 1.0f;
}
#endif

float ph_display_content_scale(SDL_Window *window)
{
#if defined(PH_HAVE_X11) && defined(SDL_VIDEO_DRIVER_X11)
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window && SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_X11) {
        Display *display = info.info.x11.display;
        if (!display)
            return 1.0f;
        Atom type;
        int format;
        unsigned long count, remaining;
        unsigned char *resources = NULL;
        float scale = 1.0f;
        /* Read the live setting. XResourceManagerString caches it for the display connection. */
        int result = XGetWindowProperty(display, DefaultRootWindow(display), XA_RESOURCE_MANAGER,
                                       0, 16384, False, XA_STRING, &type, &format, &count,
                                       &remaining, &resources);
        if (result == Success && type == XA_STRING && format == 8 && resources)
            scale = x11_content_scale((const char *)resources);
        if (resources)
            XFree(resources);
        return scale;
    }
#else
    (void)window;
#endif
    /* Windows DPI-scaled points and Wayland already have the appropriate window units. */
    return 1.0f;
}
