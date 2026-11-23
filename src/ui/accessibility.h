#ifndef PORTHOLE_UI_ACCESSIBILITY_H
#define PORTHOLE_UI_ACCESSIBILITY_H

#ifdef _WIN32
#include "ui/workspace.h"

typedef struct ph_accessibility ph_accessibility;
typedef void (*ph_accessibility_action)(void *context);

ph_accessibility *ph_accessibility_attach(SDL_Window *window, ph_accessibility_action run_action,
                                          void *context);
void ph_accessibility_update(ph_accessibility *access, ph_workspace *workspace);
void ph_accessibility_detach(ph_accessibility *access);
#endif

#endif
