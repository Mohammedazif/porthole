#ifndef PORTHOLE_UI_CONTROL_ICONS_H
#define PORTHOLE_UI_CONTROL_ICONS_H
#include <SDL.h>
typedef enum {
    PH_ICON_HOME,
    PH_ICON_APPS,
    PH_ICON_SLIDERS,
    PH_ICON_BELL,
    PH_ICON_SHOT,
    PH_ICON_VOLUP,
    PH_ICON_VOLDOWN,
    PH_ICON_FEED,
    PH_ICON_GEAR,
    PH_ICON_SEND,
    PH_ICON_RECEIVE,
    PH_ICON_TRANSFER,
    PH_ICON_PHONE,
    PH_ICON_MOUSE,
    PH_ICON_CLIPBOARD,
    PH_ICON_WIFI,
    PH_ICON_FOLDER,
    PH_ICON_CHEVRON,
    PH_ICON_COPY,
    PH_ICON_CLOSE,
    PH_ICON_FILE,
    PH_ICON_SUN,
    PH_ICON_ARROWS,
    PH_ICON_HELP,
    PH_ICON_STATS,
    PH_ICON_INFO,
    PH_ICON_SEARCH,
    PH_ICON_CHECK,
    PH_ICON_DOWN,
    PH_ICON_DEVICES,
} ph_control_icon;
void ph_icon_draw(SDL_Renderer *renderer, ph_control_icon kind, SDL_Rect box, SDL_Color color);
void ph_icon_release(SDL_Renderer *renderer);
#endif
