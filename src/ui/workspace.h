#ifndef PORTHOLE_UI_WORKSPACE_H
#define PORTHOLE_UI_WORKSPACE_H
#include <SDL.h>
#include "core/metrics.h"
#include "files/history.h"
#include "files/origin.h"
#include "session/devices.h"

typedef struct ph_workspace ph_workspace;
#define PH_WORKSPACE_MAX_CONTROLS 512
typedef enum ph_workspace_view {
    PH_WORKSPACE_HOME,
    PH_WORKSPACE_SETTINGS,
    PH_WORKSPACE_HISTORY,
    PH_WORKSPACE_CLIPBOARD,
    PH_WORKSPACE_FILES
} ph_workspace_view;
enum { PH_SETTINGS_CONTROLS = 2, PH_SETTINGS_HELP = 5, PH_SETTINGS_DEVICES = 7,
       PH_SETTINGS_ABOUT, PH_SETTINGS_COUNT };
typedef enum ph_workspace_action {
    PH_WORKSPACE_NONE,
    PH_WORKSPACE_CLOSE,
    PH_WORKSPACE_SETTINGS_OPEN,
    PH_WORKSPACE_SETUP,
    PH_WORKSPACE_SEND_FILES,
    PH_WORKSPACE_FOLDER,
    PH_WORKSPACE_NEW_CODE,
    PH_WORKSPACE_CONTROLS,
    PH_WORKSPACE_FEED,
    PH_WORKSPACE_CTRL_COMMAND,
    PH_WORKSPACE_MEDIA,
    PH_WORKSPACE_RELEASE,
    PH_WORKSPACE_POINTER,
    PH_WORKSPACE_TOOLBAR,
    PH_WORKSPACE_SOUND,
    PH_WORKSPACE_VOLUME_UP,
    PH_WORKSPACE_VOLUME_DOWN,
    PH_WORKSPACE_CAPTURE,
    PH_WORKSPACE_RECORD,
    PH_WORKSPACE_FULLSCREEN,
    PH_WORKSPACE_FORGET,
    PH_WORKSPACE_DISCONNECT,
    PH_WORKSPACE_CLIP_COPY,
    PH_WORKSPACE_CLIP_SEND,
    PH_WORKSPACE_LOCATE,
    PH_WORKSPACE_REVEAL,
    PH_WORKSPACE_STATS,
    PH_WORKSPACE_HISTORY_OPEN,
    PH_WORKSPACE_CLIPBOARD_OPEN,
    PH_WORKSPACE_COPY_ADDRESS,
    PH_WORKSPACE_COPY_CODE,
    PH_WORKSPACE_HIDE,
    PH_WORKSPACE_QUIT,
    PH_WORKSPACE_RETRY,
    PH_WORKSPACE_RESTART,
    PH_WORKSPACE_DEVICE_SELECT,
    PH_WORKSPACE_DEVICE_RENAME,
    PH_WORKSPACE_DEVICE_DISCONNECT,
    PH_WORKSPACE_DEVICE_FORGET,
    PH_WORKSPACE_PIN_APPROVE,
    PH_WORKSPACE_PIN_REJECT,
    PH_WORKSPACE_PIN_REQUIRED,
    PH_WORKSPACE_CANCEL_SEND,
    PH_WORKSPACE_CAPTURES,
    PH_WORKSPACE_CLEAR_ALL_CLIPBOARD,
    PH_WORKSPACE_ADD_PHONE,
    PH_WORKSPACE_RETRY_PAIRING,
    PH_WORKSPACE_ALWAYS_ON_TOP
} ph_workspace_action;
typedef struct ph_workspace_live {
    char bluetooth_peer[128];
    bool paired, connected, mirroring, controls, paused, feed, toolbar, ctrl_command, media_keys,
        release_blur, relative, sound, recording;
    bool share_checked, share_available, clipboard_available, modal;
    ph_origin_start_status pairing_status;
    bool always_on_top, controls_connected, phone_controls_managed, phone_controls_allowed,
         phone_controls_verified,
        phone_controls_screen_only;
    ph_metrics_snapshot metrics;
    char pc[64], phone[128], address[96], code[40], folder[128], detail[256], active_file[128];
    char incoming_file[128];
    uint64_t incoming_bytes, incoming_size;
    int percent, receiving_done, receiving_total, volume;
    unsigned unread_files;
} ph_workspace_live;

#ifdef _WIN32
typedef struct ph_workspace_control {
    SDL_Rect bounds;
    int action;
    uint64_t item, revision;
    char name[96];
    char value[256];
    bool editable, toggle, checked, focused, sensitive;
} ph_workspace_control;

int ph_workspace_accessible_count(const ph_workspace *ui);
bool ph_workspace_accessible_control(const ph_workspace *ui, int index, ph_workspace_control *out);
bool ph_workspace_accessible_invoke(ph_workspace *ui, const ph_workspace_control *control,
                                    bool focus_only, const char *value);
#endif

ph_workspace *ph_workspace_create(void);
void ph_workspace_destroy(ph_workspace *ui);
void ph_workspace_set_history(ph_workspace *ui, ph_history *history);
void ph_workspace_draw(ph_workspace *ui, SDL_Renderer *renderer, SDL_Rect rect,
                       ph_workspace_view view, const ph_workspace_live *live);
/* Handles only the rendered local region; never injects phone input. */
bool ph_workspace_event(ph_workspace *ui, const SDL_Event *event);
bool ph_workspace_owns_keyboard(const ph_workspace *ui);
ph_workspace_action ph_workspace_take_action(ph_workspace *ui, uint64_t *item);
bool ph_workspace_dark(ph_workspace *ui);
bool ph_workspace_tray(const ph_workspace *ui);
bool ph_workspace_notifications(const ph_workspace *ui);
bool ph_workspace_keep_aspect(const ph_workspace *ui);
bool ph_workspace_clip_send(const ph_workspace *ui);
bool ph_workspace_clip_receive(const ph_workspace *ui);
int ph_workspace_current_section(const ph_workspace *ui);
void ph_workspace_refresh_files(ph_workspace *ui);
bool ph_workspace_set_sessions(ph_workspace *ui, const ph_session_state *sessions);
const char *ph_workspace_device(const ph_workspace *ui);
const char *ph_workspace_value(const ph_workspace *ui);
void ph_workspace_open_section(ph_workspace *ui, int section);
void ph_workspace_open_history(ph_workspace *ui, bool received);
bool ph_workspace_target(ph_workspace *ui, ph_workspace_action action, int *x, int *y);

#endif
