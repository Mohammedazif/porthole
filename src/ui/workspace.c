#include "ui/workspace.h"
#include "ui/control_icons.h"
#include "ui/text.h"
#include "ui/vendor/qrcodegen.h"
#include "core/config.h"
#include "core/secure_link.h"
#include "core/upath.h"
#include "core/version.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

typedef struct ui_hit {
    SDL_Rect box;
    int action;
    uint64_t id, revision;
    char name[96];
} ui_hit;
enum {
    UI_SECTION = 100,
    UI_THEME = 120,
    UI_FILTER = 130,
    UI_SELECT = 140,
    UI_COPY_ADDRESS,
    UI_COPY_CODE,
    UI_HISTORY,
    UI_SEARCH,
    UI_CLIP_CHECK,
    UI_CLIP_REMOVE,
    UI_CLIP_DELETE,
    UI_CLEAR,
    UI_CONFIRM,
    UI_CANCEL,
    UI_TRAY,
    UI_NOTIFY,
    UI_SYNC,
    UI_TO_PHONE,
    UI_TO_PC,
    UI_GUIDES,
    UI_SHORTCUTS,
    UI_DATE,
    UI_RENAME,
    UI_DISCONNECT,
    UI_FORGET,
    UI_REVIEW,
    UI_SELECT_DEVICE,
    UI_REJECT,
    UI_EDIT_DEVICE,
    UI_SECTION_MENU,
    UI_FINGERPRINT,
    UI_CLEAR_ALL_CLIPBOARD
};
struct ph_workspace {
    ph_history *history;
    SDL_Renderer *renderer;
    SDL_Rect rect;
    ph_workspace_view view;
    ph_workspace_live live;
    ui_hit hits[PH_WORKSPACE_MAX_CONTROLS];
    ui_hit pressed, focused;
    int hits_n, section, filter, scroll, theme, confirm;
    int scroll_limit;
    uint64_t revision;
    size_t cursor, anchor;
    ph_session_state sessions;
    char device_id[129], edit_value[129];
    bool section_menu;
    int date_range;
    struct {
        uint64_t id;
        ph_file_availability value;
    } availability[64];
    uint64_t selected, clip_selected[100];
    size_t clip_selected_n;
    bool history_open, editing, tray, notifications, sync, to_phone, to_pc, shortcuts;
    char search[256];
    char error[128];
    ph_workspace_action action;
    uint64_t action_id;
    atomic_bool system_dark, stop;
    pthread_t theme_thread;
    bool theme_started;
    SDL_Color ink, muted, surface, background, border, selected_bg, positive;
};
static void color(SDL_Renderer *r, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}
static void rect(ph_workspace *u, SDL_Rect b, SDL_Color c)
{
    color(u->renderer, c);
    SDL_RenderFillRect(u->renderer, &b);
}
static void rounded(ph_workspace *u, SDL_Rect b, int radius, SDL_Color c)
{
    color(u->renderer, c);
    for (int y = 0; y < b.h; y++) {
        int d = y < radius ? radius - y - 1 : y >= b.h - radius ? y - (b.h - radius) : 0;
        int inset = d > 0 ? radius - (int)sqrt((double)(radius * radius - d * d)) : 0;
        SDL_RenderDrawLine(u->renderer, b.x + inset, b.y + y, b.x + b.w - inset - 1, b.y + y);
    }
}
static int text(ph_workspace *u, const char *s, int x, int y, int width, int size, bool bold,
                SDL_Color c)
{
    return ph_text_draw(u->renderer, s, x, y, width,
                        PH_TEXT(size, bold ? PH_FONT_MEDIUM : PH_FONT_REGULAR), c);
}
static void fitted_label(ph_workspace *u, const char *s, int x, int y, int width, int size,
                         bool bold, SDL_Color c)
{
    if (width < 12)
        return;
    int font = PH_TEXT(size, bold ? PH_FONT_MEDIUM : PH_FONT_REGULAR);
    char buf[320];
    size_t n = strcspn(s, "\r\n");
    bool truncated = s[n] != 0 || n >= sizeof buf - 4;
    if (n >= sizeof buf - 4) {
        n = sizeof buf - 4;
        while (n && ((unsigned char)s[n] & 192) == 128)
            n--;
    }
    memcpy(buf, s, n);
    buf[n] = 0;
    while (n && ph_text_width(buf, font) > width - (truncated ? ph_text_width("…", font) : 0)) {
        do {
            n--;
        } while (n && ((unsigned char)buf[n] & 192) == 128);
        truncated = true;
        buf[n] = 0;
    }
    if (truncated)
        strcat(buf, "…");
    text(u, buf, x, y, width, size, bold, c);
}
static void line(ph_workspace *u, int x, int y, int width)
{
    rect(u, (SDL_Rect){x, y, width, 1}, u->border);
}
static void card(ph_workspace *u, SDL_Rect b)
{
    rounded(u, b, 12, u->border);
    b.x++;
    b.y++;
    b.w -= 2;
    b.h -= 2;
    rounded(u, b, 11, u->surface);
}
static void hit(ph_workspace *u, SDL_Rect b, int action, uint64_t id, const char *name)
{
    SDL_Rect clipped;
    if (SDL_RenderIsClipEnabled(u->renderer)) {
        SDL_Rect viewport;
        SDL_RenderGetClipRect(u->renderer, &viewport);
        if (!SDL_IntersectRect(&b, &viewport, &clipped))
            return;
        b = clipped;
    }
    if (u->hits_n < (int)SDL_arraysize(u->hits) && SDL_IntersectRect(&b, &u->rect, &clipped)) {
        ui_hit *target = &u->hits[u->hits_n++];
        *target = (ui_hit){.box = clipped, .action = action, .id = id,
                           .revision = u->revision};
        snprintf(target->name, sizeof target->name, "%s", name);
    }
}

static bool same_hit(const ui_hit *a, const ui_hit *b)
{
    return a->action == b->action && a->id == b->id && a->revision == b->revision &&
           !strcmp(a->name, b->name);
}

static int focused_index(const ph_workspace *u)
{
    for (int i = 0; i < u->hits_n; i++)
        if (u->hits[i].action == u->focused.action && u->hits[i].id == u->focused.id &&
            u->hits[i].revision == u->focused.revision)
            return i;
    return -1;
}

static void button(ph_workspace *u, SDL_Rect b, const char *label, int action, uint64_t id,
                   bool primary, bool enabled)
{
    SDL_Color bg = primary ? u->ink : u->surface, fg = primary ? u->surface : u->ink;
    if (!enabled) {
        bg = u->selected_bg;
        fg = u->muted;
    }
    rounded(u, b, 7, u->border);
    SDL_Rect fill = {b.x + 1, b.y + 1, b.w - 2, b.h - 2};
    rounded(u, fill, 6, bg);
    int font = PH_TEXT(12, PH_FONT_MEDIUM), tw = ph_text_width(label, font);
    int padding = SDL_max(4, (b.w - tw) / 2);
    fitted_label(u, label, b.x + padding, b.y + (b.h - ph_text_line_height(font)) / 2,
                 b.w - padding - 4, 12, true, fg);
    if (enabled)
        hit(u, b, action, id, label);
}
static void copy_button(ph_workspace *u, SDL_Rect b, int action, bool enabled)
{
    color(u->renderer, enabled ? u->ink : u->muted);
    SDL_Rect page = {b.x + 11, b.y + 10, 10, 12}, back = {b.x + 8, b.y + 7, 10, 12};
    SDL_RenderDrawRect(u->renderer, &back);
    rect(u, page, u->surface);
    color(u->renderer, enabled ? u->ink : u->muted);
    SDL_RenderDrawRect(u->renderer, &page);
    if (enabled)
        hit(u, b, action, 0, action == PH_WORKSPACE_COPY_ADDRESS ? "Copy address"
                            : action == PH_WORKSPACE_COPY_CODE ? "Copy connection code"
                                                              : "Copy certificate fingerprint");
}
static void close_button(ph_workspace *u, int x, int y, int action)
{
    SDL_Rect b = {x, y, 28, 28};
    color(u->renderer, u->muted);
    SDL_RenderDrawLine(u->renderer, x + 9, y + 9, x + 19, y + 19);
    SDL_RenderDrawLine(u->renderer, x + 19, y + 9, x + 9, y + 19);
    hit(u, b, action, 0, action == PH_WORKSPACE_CANCEL_SEND ? "Cancel send" : "Close");
}
static void status_dot(ph_workspace *u, int x, int y, bool good)
{
    rounded(u, (SDL_Rect){x, y, 6, 6}, 3, good ? u->positive : u->muted);
}
static void toggle(ph_workspace *u, int x, int y, const char *name, bool on, int action,
                   bool enabled)
{
    rounded(u, (SDL_Rect){x, y, 36, 20}, 10, on ? u->ink : u->border);
    rounded(u, (SDL_Rect){x + (on ? 18 : 2), y + 2, 16, 16}, 8, u->surface);
    if (enabled) {
        char label[96];
        snprintf(label, sizeof label, "%s (%s)", name, on ? "on" : "off");
        hit(u, (SDL_Rect){x - 8, y - 8, 52, 36}, action, 0, label);
    }
}
static void phone_icon(ph_workspace *u, int x, int y)
{
    rounded(u, (SDL_Rect){x, y, 18, 30}, 4, u->ink);
    rounded(u, (SDL_Rect){x + 2, y + 3, 14, 23}, 2, u->surface);
    rounded(u, (SDL_Rect){x + 6, y + 26, 6, 2}, 1, u->ink);
}
static bool system_preference(void)
{
#ifdef _WIN32
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    return light == 0;
#else
    const char *gtk = getenv("GTK_THEME");
    if (gtk && (strstr(gtk, "dark") || strstr(gtk, "Dark")))
        return true;
    FILE *pipe = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
    char value[256] = "";
    if (pipe) {
        if (!fgets(value, sizeof value, pipe))
            value[0] = 0;
        pclose(pipe);
    }
    if (strstr(value, "prefer-dark"))
        return true;
    if (strstr(value, "prefer-light"))
        return false;
    pipe = popen("gsettings get org.cinnamon.desktop.interface gtk-theme 2>/dev/null", "r");
    if (pipe) {
        if (!fgets(value, sizeof value, pipe))
            value[0] = 0;
        pclose(pipe);
    }
    return strstr(value, "Dark") || strstr(value, "dark");
#endif
}
static void *watch_theme(void *data)
{
    ph_workspace *u = data;
    while (!atomic_load(&u->stop)) {
        atomic_store(&u->system_dark, system_preference());
        for (int i = 0; i < 80 && !atomic_load(&u->stop); i++)
            SDL_Delay(25);
    }
    return NULL;
}
bool ph_workspace_dark(ph_workspace *u)
{
    return u && (u->theme == 1 || (u->theme == 2 && atomic_load(&u->system_dark)));
}
static void palette(ph_workspace *u)
{
    bool dark = ph_workspace_dark(u);
    u->ink = dark ? (SDL_Color){233, 236, 242, 255} : (SDL_Color){22, 27, 34, 255};
    u->muted = dark ? (SDL_Color){157, 170, 187, 255} : (SDL_Color){101, 114, 133, 255};
    u->surface = dark ? (SDL_Color){30, 35, 42, 255} : (SDL_Color){255, 255, 255, 255};
    u->background = dark ? (SDL_Color){21, 25, 30, 255} : (SDL_Color){243, 244, 246, 255};
    u->border = dark ? (SDL_Color){55, 63, 75, 255} : (SDL_Color){220, 225, 233, 255};
    u->selected_bg = dark ? (SDL_Color){49, 58, 70, 255} : (SDL_Color){233, 237, 243, 255};
    u->positive = dark ? (SDL_Color){85, 206, 154, 255} : (SDL_Color){21, 128, 61, 255};
}
static void save_preferences(ph_workspace *u)
{
    char value[96];
    snprintf(value, sizeof value, "%d %d %d %d %d %d", u->theme, u->tray, u->notifications, u->sync,
             u->to_phone, u->to_pc);
    ph_config_write_line("desktop-preferences", value);
}
ph_workspace *ph_workspace_create(void)
{
    ph_workspace *u = calloc(1, sizeof *u);
    char value[96];
    int theme = 2, tray = 1, notify = 1, sync = 1, to = 1, from = 1;
    if (!u)
        return NULL;
    if (ph_config_read_line("desktop-preferences", value, sizeof value))
        if (sscanf(value, "%d %d %d %d %d %d", &theme, &tray, &notify, &sync, &to, &from) != 6)
            theme = 2;
    u->theme = theme >= 0 && theme <= 2 ? theme : 2;
    u->tray = tray != 0;
    u->notifications = notify != 0;
    u->sync = sync != 0;
    u->to_phone = to != 0;
    u->to_pc = from != 0;
    atomic_init(&u->stop, false);
    atomic_init(&u->system_dark, false);
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!driver || strcmp(driver, "dummy"))
        u->theme_started = pthread_create(&u->theme_thread, NULL, watch_theme, u) == 0;
    palette(u);
    return u;
}
void ph_workspace_destroy(ph_workspace *u)
{
    if (u) {
        atomic_store(&u->stop, true);
        if (u->theme_started)
            pthread_join(u->theme_thread, NULL);
        free(u);
    }
}
void ph_workspace_refresh_files(ph_workspace *u)
{
    if (u)
        memset(u->availability, 0, sizeof u->availability);
}
void ph_workspace_set_history(ph_workspace *u, ph_history *h)
{
    if (u) {
        u->history = h;
        u->revision++;
        u->selected = 0;
        u->clip_selected_n = 0;
        u->scroll = 0;
        u->pressed = (ui_hit){0};
        u->focused = (ui_hit){0};
        u->error[0] = 0;
        u->confirm = 0;
        if (u->editing) {
            SDL_StopTextInput();
            u->editing = false;
        }
        ph_workspace_refresh_files(u);
    }
}
static ph_file_availability availability(ph_workspace *u, const ph_file_entry *f)
{
    size_t slot = f->id % 64;
    if (u->availability[slot].id != f->id) {
        u->availability[slot].id = f->id;
        u->availability[slot].value = ph_history_file_availability(f);
    }
    return u->availability[slot].value;
}
bool ph_workspace_tray(const ph_workspace *u)
{
    return u && u->tray;
}
bool ph_workspace_notifications(const ph_workspace *u)
{
    return !u || u->notifications;
}
bool ph_workspace_clip_send(const ph_workspace *u)
{
    return !u || (u->sync && u->to_phone);
}
bool ph_workspace_clip_receive(const ph_workspace *u)
{
    return !u || (u->sync && u->to_pc);
}
int ph_workspace_current_section(const ph_workspace *u)
{
    return u ? u->section : -1;
}
void ph_workspace_open_section(ph_workspace *u, int section)
{
    if (u) {
        u->section = section;
        u->section_menu = false;
        u->scroll = 0;
        if (u->editing)
            SDL_StopTextInput();
        u->editing = false;
        u->cursor = 0;
        u->anchor = 0;
        u->search[0] = 0;
    }
}

void ph_workspace_open_history(ph_workspace *u, bool received)
{
    if (!u)
        return;
    ph_workspace_refresh_files(u);
    u->history_open = true;
    u->selected = 0;
    u->filter = received ? 2 : 0;
    u->date_range = 0;
    u->scroll = 0;
    u->search[0] = 0;
}

static void queue(ph_workspace *u, ph_workspace_action action, uint64_t id)
{
    u->action = action;
    u->action_id = id;
}
static void size_text(uint64_t bytes, char out[40])
{
    if (bytes >= 1073741824)
        snprintf(out, 40, "%.1f GB", (double)bytes / 1073741824);
    else if (bytes >= 1048576)
        snprintf(out, 40, "%.1f MB", (double)bytes / 1048576);
    else
        snprintf(out, 40, "%.0f KB", (double)bytes / 1024);
}
static void timestamp(int64_t stamp, char out[40])
{
    time_t value = (time_t)stamp;
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &value);
#else
    localtime_r(&value, &t);
#endif
    strftime(out, 40, "%d %b %Y · %H:%M", &t);
}
static bool contains(const char *haystack, const char *needle)
{
    if (!needle[0])
        return true;
    for (; *haystack; haystack++) {
        size_t n = 0;
        while (needle[n] && haystack[n] &&
               tolower((unsigned char)haystack[n]) == tolower((unsigned char)needle[n]))
            n++;
        if (!needle[n])
            return true;
    }
    return false;
}
static void qr(ph_workspace *u, SDL_Rect box)
{
    uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(8)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    char uri[240];
    const char *pin = ph_tls_fingerprint();
    if (!*pin)
        return;
    snprintf(uri, sizeof uri, "porthole://%s/%s?pin=%s", u->live.address, u->live.code, pin);
    if (!qrcodegen_encodeText(uri, tmp, code, qrcodegen_Ecc_MEDIUM, 1, 8, qrcodegen_Mask_AUTO,
                              true))
        return;
    int n = qrcodegen_getSize(code), scale = box.w / (n + 8);
    if (scale < 1)
        return;
    int x = box.x + (box.w - scale * (n + 8)) / 2, y = box.y + (box.h - scale * (n + 8)) / 2;
    rect(u, box, (SDL_Color){255, 255, 255, 255});
    for (int cy = 0; cy < n; cy++)
        for (int cx = 0; cx < n; cx++)
            if (qrcodegen_getModule(code, cx, cy))
                rect(u, (SDL_Rect){x + (cx + 4) * scale, y + (cy + 4) * scale, scale, scale},
                     (SDL_Color){17, 17, 17, 255});
}
static size_t previous_char(const char *s, size_t at)
{
    if (at)
        do {
            at--;
        } while (at && ((unsigned char)s[at] & 192) == 128);
    return at;
}

static size_t next_char(const char *s, size_t at)
{
    if (s[at])
        do {
            at++;
        } while (((unsigned char)s[at] & 192) == 128);
    return at;
}

static int text_span_width(const char *s, size_t bytes, int font)
{
    char part[256];
    if (bytes >= sizeof part)
        return 0;
    memcpy(part, s, bytes);
    part[bytes] = 0;
    return ph_text_width(part, font);
}

static void entry_box(ph_workspace *u, SDL_Rect box, const char *value, const char *label,
                      int action)
{
    rounded(u, box, 7, u->selected_bg);
    int x = box.x + 10;
    int width = box.w - 20;
    if (u->editing && action == (u->confirm ? UI_EDIT_DEVICE : UI_SEARCH)) {
        int font = PH_TEXT(12, PH_FONT_REGULAR);
        size_t start = 0, cursor = SDL_min(u->cursor, strlen(value));
        while (start < cursor && text_span_width(value + start, cursor - start, font) > width - 2)
            start = next_char(value, start);
        size_t lo = SDL_max(start, SDL_min(u->cursor, u->anchor));
        size_t hi = SDL_max(u->cursor, u->anchor);
        if (lo < hi) {
            int from = text_span_width(value + start, lo - start, font);
            int to = text_span_width(value + start, hi - start, font);
            rect(u, (SDL_Rect){x + from, box.y + 7, SDL_min(to, width) - from, box.h - 14},
                 ph_workspace_dark(u) ? (SDL_Color){55, 75, 103, 255}
                                      : (SDL_Color){170, 199, 242, 255});
        }
        fitted_label(u, value + start, x, box.y + 9, width, 12, false, u->ink);
        int at = x + text_span_width(value + start, cursor - start, font);
        rect(u, (SDL_Rect){at, box.y + 7, 1, box.h - 14}, u->ink);
        SDL_SetTextInputRect(&box);
    } else {
        fitted_label(u, value[0] ? value : label, x, box.y + 9, width, 12, false,
                     value[0] ? u->ink : u->muted);
    }
    hit(u, box, action, 0, label);
}

static void search_box(ph_workspace *u, SDL_Rect box)
{
    entry_box(u, box, u->search, "Search", UI_SEARCH);
}
static void draw_file_rows(ph_workspace *u, SDL_Rect b, bool full)
{
    const char *filters[] = {"All transfers", "Sent", "Received"};
    int x = b.x, y = b.y;
    int tabs = b.w < 390 ? (full ? b.w - 95 : b.w) : 270;
    for (int i = 0; i < 3; i++) {
        int width = i ? tabs * 28 / 100 : tabs * 44 / 100;
        text(u, filters[i], x, y + 8, width, 12, u->filter == i,
             u->filter == i ? u->ink : u->muted);
        hit(u, (SDL_Rect){x, y, width, 34}, UI_FILTER + i, 0, filters[i]);
        if (u->filter == i)
            rect(u, (SDL_Rect){x, y + 32, width - 18, 2}, u->ink);
        x += width;
    }
    if (full) {
        button(u, (SDL_Rect){b.x + b.w - 92, y, 92, 30}, "Clear history", UI_CLEAR, 0, false,
               ph_history_file_count(u->history) > 0);
    }
    line(u, b.x, y + 34, b.w);
    y += 44;
    SDL_Rect clip = {b.x, y, b.w, SDL_max(0, b.h - 44)};
    SDL_RenderSetClipRect(u->renderer, &clip);
    int cy = y - u->scroll;
    bool any = false;
    time_t cutoff = 0;
    if (full && u->date_range) {
        time_t now = time(NULL);
        struct tm *local = localtime(&now);
        if (local) {
            struct tm day = *local;
            day.tm_hour = day.tm_min = day.tm_sec = 0;
            day.tm_isdst = -1;
            if (u->date_range == 2)
                day.tm_mday -= 6;
            cutoff = mktime(&day);
        }
    }
    for (size_t i = 0; i < ph_history_file_count(u->history); i++) {
        const ph_file_entry *f = ph_history_file(u->history, i);
        if (f->time < cutoff || (u->filter == 1 && f->incoming) ||
            (u->filter == 2 && !f->incoming) ||
            (!contains(f->name, u->search) && !contains(f->path, u->search)))
            continue;
        any = true;
        if (cy + 66 > y && cy < y + clip.h) {
            SDL_Rect row = {b.x, cy, b.w, 66};
            if (u->selected == f->id)
                rounded(u, row, 6, u->selected_bg);
            char when[40], size[40], sub[128];
            timestamp(f->time, when);
            size_text(f->bytes, size);
            snprintf(sub, sizeof sub, "%s · %s · %s", size,
                     f->incoming ? "From iPhone" : "To iPhone", when);
            int right = b.w > 420 ? 120 : 86;
            ph_icon_draw(u->renderer, f->incoming ? PH_ICON_RECEIVE : PH_ICON_SEND,
                         (SDL_Rect){b.x + 10, cy + 19, 20, 20}, u->muted);
            fitted_label(u, f->name, b.x + 40, cy + 12, b.w - right - 52, 13, true, u->ink);
            fitted_label(u, sub, b.x + 40, cy + 35, b.w - right - 52, 10, false, u->muted);
            const char *state = f->state == PH_TRANSFER_COMPLETE
                                    ? (f->incoming ? "Received" : "Sent")
                                : f->state == PH_TRANSFER_CHANGED  ? "Source changed"
                                : f->state == PH_TRANSFER_FAILED   ? "Failed"
                                : f->state == PH_TRANSFER_CANCELED ? "Canceled"
                                : f->state == PH_TRANSFER_PAUSED   ? "Paused"
                                : f->state == PH_TRANSFER_ACTIVE   ? "Transferring"
                                                                   : "Queued";
            text(u, state, b.x + b.w - right, cy + 12, right - 8, 11, false,
                 f->state == PH_TRANSFER_COMPLETE ? u->positive : u->muted);
            ph_file_availability state_on_disk = availability(u, f);
            if (state_on_disk != PH_FILE_PRESENT && state_on_disk != PH_FILE_NO_PATH)
                text(u,
                     state_on_disk == PH_FILE_MISSING ? "File not found" : "Location unavailable",
                     b.x + b.w - right, cy + 35, right - 8, 10, false, u->muted);
            hit(u, row, UI_SELECT, f->id, f->name);
            line(u, b.x + 12, cy + 65, b.w - 24);
        }
        cy += 66;
    }
    SDL_RenderSetClipRect(u->renderer, NULL);
    if (!any && clip.h >= 54)
        text(u, u->search[0] ? "No matching transfers." : "No file transfers yet.", b.x + 14,
             y + 28, b.w - 28, 13, false, u->muted);
    u->scroll_limit = SDL_max(cy + u->scroll - y - clip.h, 0);
}
static const ph_file_entry *selected_file(ph_workspace *u)
{
    for (size_t i = 0; i < ph_history_file_count(u->history); i++) {
        const ph_file_entry *f = ph_history_file(u->history, i);
        if (f->id == u->selected)
            return f;
    }
    return NULL;
}
static void draw_file_detail(ph_workspace *u, SDL_Rect b)
{
    const ph_file_entry *f = selected_file(u);
    if (!f)
        return;
    card(u, b);
    int x = b.x + 18, y = b.y + 18, width = b.w - 36;
    fitted_label(u, f->name, x, y, width - 26, 17, true, u->ink);
    y += 32;
    close_button(u, b.x + b.w - 38, b.y + 10, UI_CANCEL);
    char date[40];
    timestamp(f->time, date);
    y = text(u, date, x, y, width, 11, false, u->muted) + 24;
    y = text(u, f->incoming ? "SAVED LOCATION" : "SOURCE LOCATION", x, y, width, 10, true,
             u->muted) +
        9;
    fitted_label(u, f->path[0] ? f->path : "Location not recorded", x, y, width, 11, false, u->ink);
    y += 26;
    if (f->incoming && f->state == PH_TRANSFER_PAUSED)
        y = text(u, "Retry from your iPhone to resume this transfer.", x, y, width, 12, false,
                 u->muted) +
            18;
    ph_file_availability state = availability(u, f);
    bool can_locate = state == PH_FILE_MISSING || state == PH_FILE_LOCATION_UNAVAILABLE;
    if (can_locate) {
        y = text(u, state == PH_FILE_MISSING ? "File not found" : "Location unavailable", x, y,
                 width, 13, true, u->ink) +
            8;
        y = text(u,
                 state == PH_FILE_MISSING ? "The file may have been moved, renamed, or deleted."
                                          : "Reconnect the drive or locate the file.",
                 x, y, width, 12, false, u->muted) +
            18;
    }
    button(u, (SDL_Rect){x, y, width < 230 ? width : 160, 34},
           can_locate ? "Locate file…" : "Show in folder",
           can_locate ? PH_WORKSPACE_LOCATE : PH_WORKSPACE_REVEAL, f->id, false,
           can_locate || state == PH_FILE_PRESENT);
    if (!f->incoming && state == PH_FILE_PRESENT &&
        (f->state == PH_TRANSFER_CHANGED || f->state == PH_TRANSFER_FAILED ||
         f->state == PH_TRANSFER_PAUSED || f->state == PH_TRANSFER_CANCELED)) {
        y += 48;
        button(u, (SDL_Rect){x, y, width < 230 ? width : 180, 32},
               f->state == PH_TRANSFER_CHANGED ? "Restart transfer" : "Retry transfer",
               f->state == PH_TRANSFER_CHANGED ? PH_WORKSPACE_RESTART : PH_WORKSPACE_RETRY, f->id,
               true, u->live.connected);
    }
    button(u, (SDL_Rect){x, y + 48, width < 230 ? width : 180, 32}, "Remove from history",
           UI_CLIP_REMOVE, f->id, false, true);
}
static void history_page(ph_workspace *u)
{
    SDL_Rect b = u->rect;
    int pad = b.w < 600 ? 20 : 32;
    text(u, "File history", b.x + pad, b.y + 28, b.w - 2 * pad - 40, 27, true, u->ink);
    text(u, "All recorded transfers for this iPhone, including previous days.", b.x + pad, b.y + 68,
         b.w - 2 * pad, 12, false, u->muted);
    close_button(u, b.x + b.w - pad - 28, b.y + 24, PH_WORKSPACE_CLOSE);
    search_box(u, (SDL_Rect){b.x + pad, b.y + 104, b.w - 2 * pad - 130, 36});
    button(u, (SDL_Rect){b.x + b.w - pad - 118, b.y + 104, 118, 36},
           u->date_range == 0   ? "All time"
           : u->date_range == 1 ? "Today"
                                : "Last 7 days",
           UI_DATE, 0, false, true);
    int detail = u->selected && b.w >= 850 ? 340 : 0;
    SDL_Rect list = {b.x + pad, b.y + 160, b.w - 2 * pad - detail - (detail ? 20 : 0), b.h - 184};
    if (!u->selected || detail) {
        card(u, list);
        SDL_Rect rows = {list.x + 14, list.y + 10, list.w - 28, list.h - 20};
        draw_file_rows(u, rows, true);
    }
    if (detail)
        draw_file_detail(u, (SDL_Rect){list.x + list.w + 20, list.y, detail, list.h});
    else if (u->selected)
        draw_file_detail(u, (SDL_Rect){b.x + pad, b.y + 160, b.w - 2 * pad, b.h - 184});
}
static bool clip_checked(const ph_workspace *u, uint64_t id)
{
    for (size_t i = 0; i < u->clip_selected_n; i++)
        if (u->clip_selected[i] == id)
            return true;
    return false;
}

static bool clipboard_ready(const ph_workspace *u)
{
    return u->sync && (u->to_pc || u->to_phone) && u->live.connected && u->live.clipboard_available;
}

static const char *clipboard_status(const ph_workspace *u, bool short_label)
{
    if (!u->sync || (!u->to_pc && !u->to_phone))
        return short_label ? "Off" : "Clipboard sync is off";
    if (!u->live.connected)
        return short_label ? "Disconnected" : "Reconnect your iPhone to sync";
    if (!u->live.clipboard_available)
        return short_label ? "Unavailable" : "Enable clipboard sync on your iPhone";
    if (!u->to_pc)
        return "PC to iPhone";
    if (!u->to_phone)
        return "iPhone to PC";
    return short_label ? "Enabled" : "Clipboard sync is on";
}

/* Clipboard images are PNG. Reading the header avoids decoding in the renderer. */
static void image_preview(const ph_clip_entry *item, char *label, size_t capacity)
{
    char bytes[40];
    size_text(item->size, bytes);
    const unsigned char *d = item->data;
    if (item->size >= 24 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8) && !memcmp(d + 12, "IHDR", 4)) {
        uint32_t width = (uint32_t)d[16] << 24 | (uint32_t)d[17] << 16 | (uint32_t)d[18] << 8 | d[19];
        uint32_t height = (uint32_t)d[20] << 24 | (uint32_t)d[21] << 16 | (uint32_t)d[22] << 8 | d[23];
        snprintf(label, capacity, "Image · %u × %u · %s", (unsigned)width, (unsigned)height, bytes);
    } else {
        snprintf(label, capacity, "Image · %s", bytes);
    }
}

static void clipboard_page(ph_workspace *u)
{
    SDL_Rect b = u->rect;
    int x = b.x + 16, y = b.y + 15, width = b.w - 32;
    size_t kept = 0;
    for (size_t i = 0; i < u->clip_selected_n; i++) {
        for (size_t j = 0; j < ph_history_clip_count(u->history); j++)
            if (ph_history_clip(u->history, j)->id == u->clip_selected[i]) {
                u->clip_selected[kept++] = u->clip_selected[i];
                break;
            }
    }
    u->clip_selected_n = kept;
    text(u, "Clipboard", x, y, width - 32, 20, true, u->ink);
    close_button(u, b.x + b.w - 42, y - 3, PH_WORKSPACE_CLOSE);
    y += 45;
    bool ready = clipboard_ready(u);
    status_dot(u, x, y + 5, ready);
    fitted_label(u, clipboard_status(u, false), x + 13, y, width - 13, 12, false,
                 ready ? u->positive : u->muted);
    y += 33;
    char count[64];
    snprintf(count, sizeof count, "Clipboard history · %zu", ph_history_clip_count(u->history));
    text(u, count, x, y, width - 92, 12, true, u->ink);
    button(u, (SDL_Rect){x + width - 88, y - 6, 88, 28}, "Clear history", UI_CLEAR, 0, false,
           ph_history_clip_count(u->history) > 0);
    y += 35;
    button(u, (SDL_Rect){x, y, 76, 27},
           u->clip_selected_n && u->clip_selected_n == ph_history_clip_count(u->history)
               ? "Deselect all"
               : "Select all",
           UI_CLIP_CHECK, 0, false, ph_history_clip_count(u->history) > 0);
    button(u, (SDL_Rect){x + width - 116, y, 116, 27}, "Delete selected", UI_CLIP_DELETE, 0, false,
           u->clip_selected_n > 0);
    y += 38;
    SDL_Rect clip = {x, y, width, SDL_max(0, b.y + b.h - y - 48)};
    SDL_RenderSetClipRect(u->renderer, &clip);
    int cy = y - u->scroll;
    for (size_t i = 0; i < ph_history_clip_count(u->history); i++) {
        const ph_clip_entry *c = ph_history_clip(u->history, i);
        if (cy + 110 > clip.y && cy < clip.y + clip.h) {
            if (clip_checked(u, c->id))
                rounded(u, (SDL_Rect){x, cy, width, 106}, 6, u->selected_bg);
            SDL_Rect check = {x + 2, cy + 13, 13, 13};
            rounded(u, check, 3, clip_checked(u, c->id) ? u->ink : u->border);
            char preview[200], date[40];
            timestamp(c->time, date);
            if (c->kind == 'T')
                snprintf(preview, sizeof preview, "%.190s", (const char *)c->data);
            else
                image_preview(c, preview, sizeof preview);
            hit(u, (SDL_Rect){x, cy, width, 32}, UI_CLIP_CHECK, c->id,
                clip_checked(u, c->id) ? "Deselect clipboard item" : "Select clipboard item");
            fitted_label(u, preview, x + 24, cy + 10, width - 28, 12, false, u->ink);
            char sub[80];
            snprintf(sub, sizeof sub, "%s · %s", c->incoming ? "From iPhone" : "From this PC",
                     date);
            text(u, sub, x + 24, cy + 39, width - 28, 10, false, u->muted);
            button(u, (SDL_Rect){x, cy + 66, 58, 28}, "Copy", PH_WORKSPACE_CLIP_COPY, c->id,
                   false, true);
            button(u, (SDL_Rect){x + 66, cy + 66, 106, 28}, "Send to iPhone",
                   PH_WORKSPACE_CLIP_SEND, c->id, false, ready && u->to_phone);
            button(u, (SDL_Rect){x + width - 63, cy + 66, 60, 28}, "Delete", UI_CLIP_REMOVE, c->id,
                   false, true);
            line(u, x, cy + 109, width);
        }
        cy += 110;
    }
    SDL_RenderSetClipRect(u->renderer, NULL);
    if (!ph_history_clip_count(u->history))
        text(u, "No clipboard history.\nCopied text and images appear here.", x, y + 26, width, 13,
             false, u->muted);
    u->scroll_limit = SDL_max(cy + u->scroll - y - clip.h, 0);
    text(u, "Up to 100 recent items · Stored on this PC", x, b.y + b.h - 31, width, 10, false,
         u->muted);
}
static int incoming_progress(ph_workspace *u, int x, int y, int width)
{
    if (!u->live.incoming_file[0])
        return y;
    char done[40], total[40], label[128];
    size_text(u->live.incoming_bytes, done);
    size_text(u->live.incoming_size, total);
    ph_icon_draw(u->renderer, PH_ICON_RECEIVE, (SDL_Rect){x, y + 4, 21, 21}, u->positive);
    fitted_label(u, u->live.incoming_file, x + 32, y, width - 32, 13, true, u->ink);
    snprintf(label, sizeof label, "Receiving · %s of %s", done, total);
    text(u, label, x + 32, y + 21, width - 32, 11, false, u->muted);
    rounded(u, (SDL_Rect){x, y + 43, width, 4}, 2, u->border);
    int filled = u->live.incoming_size
                     ? (int)(width * ((double)u->live.incoming_bytes / u->live.incoming_size))
                     : 0;
    if (filled > 0)
        rounded(u, (SDL_Rect){x, y + 43, SDL_min(width, filled), 4}, 2, u->positive);
    return y + 54;
}

static int transfer_progress(ph_workspace *u, int x, int y, int width)
{
    if (u->live.active_file[0]) {
        char progress[16];
        int percent = SDL_clamp(u->live.percent, 0, 100);
        snprintf(progress, sizeof progress, "%d%%", percent);
        fitted_label(u, u->live.active_file, x, y, width - 82, 12, true, u->ink);
        if (u->live.percent >= 0)
            text(u, progress, x + width - 76, y, 40, 11, false, u->muted);
        close_button(u, x + width - 28, y - 7, PH_WORKSPACE_CANCEL_SEND);
        rounded(u, (SDL_Rect){x, y + 23, width, 4}, 2, u->border);
        if (percent)
            rounded(u, (SDL_Rect){x, y + 23, width * percent / 100, 4}, 2, u->ink);
        y += 38;
    }
    if (u->live.incoming_file[0])
        return incoming_progress(u, x, y, width);
    if (u->live.receiving_total > 0) {
        char label[96];
        snprintf(label, sizeof label, "Receiving from iPhone · %d of %d saved",
                 u->live.receiving_done, u->live.receiving_total);
        fitted_label(u, label, x, y, width, 12, true, u->ink);
        y += 28;
    }
    return y;
}

static const char *history_button_label(const ph_workspace *u, char label[32])
{
    if (!u->live.unread_files)
        return "File history";
    snprintf(label, 32, "History (%u new)", u->live.unread_files);
    return label;
}

static void files_panel(ph_workspace *u)
{
    SDL_Rect b = u->rect;
    int inset = b.h < 220 ? 4 : 12;
    int x = b.x + 12, y = b.y + inset, width = b.w - 24;
    text(u, "File transfers", x, y, width - 40, 18, true, u->ink);
    if (u->view == PH_WORKSPACE_FILES)
        close_button(u, b.x + b.w - 42, y - 3, PH_WORKSPACE_CLOSE);
    y += 34;
    int cell = (width - 12) / 3;
    char history_name[32];
    button(u, (SDL_Rect){x, y, cell, 30}, "Send files", PH_WORKSPACE_SEND_FILES, 0,
           true, u->live.connected);
    button(u, (SDL_Rect){x + cell + 6, y, cell, 30}, history_button_label(u, history_name),
           UI_HISTORY, 0, false, true);
    button(u, (SDL_Rect){x + 2 * (cell + 6), y, cell, 30}, "Open folder", PH_WORKSPACE_REVEAL, 0,
           false, true);
    y += 44;
    y = transfer_progress(u, x, y, width);
    if (b.y + b.h > y + 90)
        draw_file_rows(u, (SDL_Rect){x, y, width, b.y + b.h - y - 12}, false);
}

static void pairing_actions(ph_workspace *u, int x, int y, int width)
{
    int small = width < 350 ? 74 : 104;
    int guide = SDL_min(150, width - 2 * small - 12);
    button(u, (SDL_Rect){x, y, guide, 34}, "Set up controls", PH_WORKSPACE_SETUP, 0, false, true);
    button(u, (SDL_Rect){x + guide + 6, y, small, 34}, "Devices", UI_SECTION + 8, 0, false, true);
    button(u, (SDL_Rect){x + guide + small + 12, y, small, 34}, "Settings",
           PH_WORKSPACE_SETTINGS_OPEN, 0, false, true);
}

static void home(ph_workspace *u)
{
    SDL_Rect b = u->rect;
    int pad = b.w < 600 ? 20 : 38, x = b.x + pad, width = b.w - 2 * pad, y = b.y + 30;
    if (!u->live.paired && !u->live.address[0] && u->live.share_checked) {
        y = text(u, "iPhone connection unavailable", x, y, width, 24, true, u->ink) + 14;
        y = text(u, "Restart Porthole and verify that your PC is connected to Wi-Fi.", x, y, width,
                 14, false, u->muted) + 24;
        pairing_actions(u, x, y, width);
        return;
    }
    if (!u->live.paired) {
        y = text(u, "Connect your iPhone", x, y, width, width < 350 ? 24 : 30, true, u->ink) + 10;
        y = text(u,
                 "Open Porthole on your iPhone and scan this code. Connect both devices to the "
                 "same Wi-Fi network.",
                 x, y, width, 14, false, u->muted) +
            26;
        if (width < 350) {
            card(u, (SDL_Rect){x, y, width, 194});
            int q = 100, dx = x + q + 20;
            if (u->live.code[0])
                qr(u, (SDL_Rect){x + 6, y + 8, q, q});
            text(u, "THIS PC", dx, y + 18, width - q - 32, 10, true, u->muted);
            fitted_label(u, u->live.pc[0] ? u->live.pc : "Porthole", dx, y + 36, width - q - 32, 15,
                         true, u->ink);
            button(u, (SDL_Rect){dx, y + 66, 90, 28}, "New code", PH_WORKSPACE_NEW_CODE, 0, false,
                   u->live.code[0] != 0);
            text(u, "Address", x + 14, y + 117, 75, 11, false, u->muted);
            fitted_label(u, u->live.address[0] ? u->live.address : "Starting…", x + 85, y + 115,
                         width - 132, 12, true, u->ink);
            copy_button(u, (SDL_Rect){x + width - 40, y + 109, 30, 28}, PH_WORKSPACE_COPY_ADDRESS,
                        u->live.address[0] != 0);
            text(u, "Code", x + 14, y + 156, 75, 11, false, u->muted);
            text(u, u->live.code, x + 85, y + 152, width - 132, 15, true, u->ink);
            copy_button(u, (SDL_Rect){x + width - 40, y + 146, 30, 28}, PH_WORKSPACE_COPY_CODE,
                        u->live.code[0] != 0);
            y += 204;
            button(u, (SDL_Rect){x, y, width, 27}, "Certificate fingerprint", UI_FINGERPRINT, 0,
                   false, true);
            y += 38;
            pairing_actions(u, x, y, width);
            return;
        }
        SDL_Rect pair = {x, y, width, 250};
        card(u, pair);
        int q = b.w < 620 ? 116 : 168, dx = x + q + 40;
        if (u->live.code[0])
            qr(u, (SDL_Rect){x + 18, y + 26, q, q});
        text(u, "THIS PC", dx, y + 24, width - q - 64, 11, true, u->muted);
        text(u, u->live.pc[0] ? u->live.pc : "Porthole", dx, y + 44, width - q - 64, 22, true,
             u->ink);
        text(u, "Address", dx, y + 84, width - q - 64, 11, false, u->muted);
        text(u, u->live.address[0] ? u->live.address : "Starting…", dx, y + 104, width - q - 94, 14,
             true, u->ink);
        copy_button(u, (SDL_Rect){x + width - 48, y + 97, 30, 28}, PH_WORKSPACE_COPY_ADDRESS,
                    u->live.address[0] != 0);
        text(u, "Connection code", dx, y + 142, width - q - 64, 11, false, u->muted);
        text(u, u->live.code, dx, y + 162, width - q - 94, 17, true, u->ink);
        copy_button(u, (SDL_Rect){x + width - 48, y + 155, 30, 28}, PH_WORKSPACE_COPY_CODE,
                    u->live.code[0] != 0);
        button(u, (SDL_Rect){dx, y + 202, 100, 32}, "New code", PH_WORKSPACE_NEW_CODE, 0, false,
               u->live.code[0] != 0);
        y += 260;
        button(u, (SDL_Rect){x, y, 170, 28}, "Certificate fingerprint", UI_FINGERPRINT, 0, false,
               true);
        y += 42;
        pairing_actions(u, x, y, width);
        return;
    }
    card(u, (SDL_Rect){x, y, 48, 58});
    phone_icon(u, x + 15, y + 14);
    fitted_label(u, u->live.phone[0] ? u->live.phone : "iPhone", x + 66, y,
                 width - (width < 550 ? 76 : 288), 28, true, u->ink);
    status_dot(u, x + 66, y + 44, u->live.connected);
    text(u, u->live.connected ? "Connected · Wi-Fi" : "Disconnected", x + 80, y + 38,
         width - (width < 550 ? 88 : 220), 12, false, u->live.connected ? u->positive : u->muted);
    int button_y = y + (width < 550 ? 65 : 10);
    button(u, (SDL_Rect){x + width - 226, button_y, 104, 34}, "Devices", UI_SECTION + 8, 0, false,
           true);
    button(u, (SDL_Rect){x + width - 110, button_y, 110, 34}, "Settings", PH_WORKSPACE_SETTINGS_OPEN,
           0, false, true);
    y += width < 550 ? 115 : 78;

    card(u, (SDL_Rect){x, y, width, 54});
    int cell = width / 3;
    const char *names[] = {"Screen sharing", "Mouse and keyboard", "Clipboard sync"};
    const char *states[] = {u->live.mirroring ? "Active"
                              : !u->live.connected ? "Disconnected"
                              : u->live.share_checked && !u->live.share_available ? "Unavailable"
                                                                                 : "Off",
                            u->live.controls && !u->live.paused ? "Active" : "Inactive",
                            clipboard_status(u, true)};
    for (int i = 0; i < 3; i++) {
        int xx = x + i * cell;
        if (i)
            rect(u, (SDL_Rect){xx, y, 1, 54}, u->border);
        const char *compact_names[] = {"Screen", "Controls", "Clipboard"};
        text(u, width < 450 ? compact_names[i] : names[i], xx + 14, y + 10, cell - 24,
             cell < 230 ? 11 : 12, true, u->ink);
        fitted_label(u, states[i], xx + 14, y + 30, cell - 24, 10, false,
                     i == 2 && clipboard_ready(u) ? u->positive : u->muted);
        hit(u, (SDL_Rect){xx, y, cell, 54},
            i == 2 ? PH_WORKSPACE_CLIPBOARD_OPEN : UI_SECTION + (i == 0 ? 1 : 2), 0, names[i]);
    }
    y += 68;
    if (!u->live.connected) {
        y = text(u, "Open Porthole on your iPhone to reconnect.", x, y, width, 14, false,
                 u->muted) +
            18;
        char history_name[32];
        button(u, (SDL_Rect){x, y, 130, 34}, history_button_label(u, history_name), UI_HISTORY, 0,
               false, true);
        return;
    }
    if (width < 450 || b.y + b.h - y < 335) {
        SDL_Rect region = u->rect;
        u->rect = (SDL_Rect){x, y, width, b.y + b.h - y};
        files_panel(u);
        u->rect = region;
        return;
    }
    int available = b.y + b.h - y - 22;
    int rows = (int)SDL_min(ph_history_file_count(u->history), 4);
    int needed = 58 + 92 + 44 + SDL_max(rows, 1) * 66 + 55;
    if (u->live.active_file[0])
        needed += 38;
    if (u->live.incoming_file[0])
        needed += 60;
    else if (u->live.receiving_total)
        needed += 30;
    SDL_Rect files = {x, y, width, SDL_min(needed, available)};
    card(u, files);
    text(u, "File transfers", x + 20, y + 18, width - 230, 18, true, u->ink);
    char history_name[32];
    button(u, (SDL_Rect){x + width - 232, y + 12, 104, 30}, history_button_label(u, history_name),
           UI_HISTORY, 0, false, true);
    button(u, (SDL_Rect){x + width - 116, y + 12, 98, 30}, "Open folder", PH_WORKSPACE_REVEAL, 0,
           false, true);
    y += 58;
    SDL_Rect drop = {x + 18, y, width - 36, 76};
    rounded(u, drop, 9, u->background);
    ph_icon_draw(u->renderer, PH_ICON_SEND, (SDL_Rect){x + 33, y + 24, 28, 28}, u->ink);
    text(u, "Send to iPhone", x + 80, y + 15, width - 244, 15, true, u->ink);
    text(u,
         u->live.connected ? "Drop files here or browse your PC."
                           : "Reconnect your iPhone to send files.",
         x + 80, y + 40, width - 244, 11, false, u->muted);
    button(u, (SDL_Rect){x + width - 124, y + 20, 92, 34}, "Send files", PH_WORKSPACE_SEND_FILES, 0,
           true, u->live.connected);
    y += 92;
    y = transfer_progress(u, x + 22, y, width - 44);
    if (files.y + files.h - y >= 89)
        draw_file_rows(u, (SDL_Rect){x + 20, y, width - 40, files.y + files.h - y - 45}, false);
    line(u, x + 1, files.y + files.h - 40, width - 2);
    char save[160];
    if (!strcmp(u->live.folder, "Folder unavailable"))
        snprintf(save, sizeof save, "Receive folder unavailable");
    else
        snprintf(save, sizeof save, "Receive folder · %s",
                 u->live.folder[0] ? u->live.folder : "Downloads");
    text(u, save, x + 20, files.y + files.h - 27, width - 160, 10, false, u->muted);
    button(u, (SDL_Rect){x + width - 126, files.y + files.h - 34, 106, 28}, "Change folder",
           PH_WORKSPACE_FOLDER, 0, false, true);
}
static int setting(ph_workspace *u, int x, int y, int width, const char *title, const char *detail,
                   int action, bool on)
{
    card(u, (SDL_Rect){x, y, width, 76});
    text(u, title, x + 16, y + 14, width - 88, 13, true, u->ink);
    text(u, detail, x + 16, y + 37, width - 88, 11, false, u->muted);
    toggle(u, x + width - 56, y + 28, title, on, action, true);
    return y + 88;
}
bool ph_workspace_set_sessions(ph_workspace *u, const ph_session_state *state)
{
    if (!u || !memcmp(&u->sessions, state, sizeof *state))
        return false;
    if (u->confirm == 10 &&
        (strcmp(u->sessions.request_id, state->request_id) || strcmp(u->sessions.pin, state->pin))) {
        u->confirm = 0;
        u->editing = false;
        SDL_StopTextInput();
    }
    /* A slot number can be reused when a device leaves or a new request appears. */
    if (u->pressed.action >= UI_RENAME && u->pressed.action <= UI_REJECT)
        u->pressed = (ui_hit){0};
    if (u->focused.action >= UI_RENAME && u->focused.action <= UI_REJECT)
        u->focused = (ui_hit){0};
    u->sessions = *state;
    u->revision++;
    return true;
}
const char *ph_workspace_device(const ph_workspace *u)
{
    return u ? u->device_id : "";
}
const char *ph_workspace_value(const ph_workspace *u)
{
    return u ? u->edit_value : "";
}
static int device_row(ph_workspace *u, int x, int y, int width, int index)
{
    const ph_session_device *d = index ? &u->sessions.devices[index - 1] : NULL;
    bool companion = !d || d->companion;
    const char *name = d ? d->name : u->live.phone;
    bool connected = d ? d->connected : u->live.connected;
    int h = width < 420 ? 148 : 126;
    card(u, (SDL_Rect){x, y, width, h});
    text(u, *name ? name : "iPhone", x + 14, y + 10, width - 28, 15, true, u->ink);
    text(u,
         companion ? "Porthole companion · Files and clipboard"
         : d->usb  ? "USB · Screen only"
                   : "AirPlay · Screen only",
         x + 14, y + 35, width - 28, 12, false, u->muted);
    text(u, connected ? "Connected" : "Disconnected", x + 14, y + 56, width - 28, 11, false,
         connected ? u->positive : u->muted);
    int dx = x + 12, dy = y + 83;
    if (d && (companion || d->usb || connected)) {
        const char *action = d->active ? "Selected" : companion ? "Use for files"
                            : d->usb && !connected            ? "Reconnect"
                                                             : "Switch view";
        button(u, (SDL_Rect){dx, dy, 92, 28}, action, UI_SELECT_DEVICE, index, false, !d->active);
        dx += 98;
    }
    if (d && d->usb) {
        if (connected)
            button(u, (SDL_Rect){dx, dy, 90, 28}, "Disconnect", UI_DISCONNECT, index, false, true);
    } else {
        button(u, (SDL_Rect){dx, dy, 66, 28}, "Rename", UI_RENAME, index, false, true);
        dx += 72;
        if (connected) {
            if (dx + 94 > x + width) {
                dx = x + 12;
                dy += 32;
            }
            button(u, (SDL_Rect){dx, dy, 90, 28}, "Disconnect", UI_DISCONNECT, index, false, true);
            dx += 96;
        }
        if (dx + 66 > x + width) {
            dx = x + 12;
            dy += 32;
        }
        button(u, (SDL_Rect){dx, dy, 62, 28}, "Forget", UI_FORGET, index, false, true);
    }
    return y + h + 12;
}
static void settings(ph_workspace *u)
{
    static const char *sections[] = {"General",
                                     "Screen and audio",
                                     "Controls",
                                     "Transfers",
                                     "Notifications",
                                     "Help and shortcuts",
                                     "Connection statistics",
                                     "About",
                                     "Devices"};
    SDL_Rect b = u->rect;
    bool narrow = b.w < 600;
    int nav = narrow ? 0 : b.w >= 650 ? 200 : 140, top = narrow ? 64 : 0;
    int x = b.x + nav + (narrow ? 16 : 24), width = b.w - nav - (narrow ? 32 : 48),
        y = b.y + top + 32 - u->scroll;
    if (narrow) {
        rect(u, (SDL_Rect){b.x, b.y, b.w, top}, u->surface);
        button(u, (SDL_Rect){b.x + 16, b.y + 13, 130, 34}, "Settings menu", UI_SECTION_MENU, 0,
               false, true);
    } else {
        rect(u, (SDL_Rect){b.x, b.y, nav, b.h}, u->surface);
        text(u, "Settings", b.x + 20, b.y + 24, nav - 35, 24, true, u->ink);
    }
    if (!narrow || u->section_menu) {
        int menu_width = narrow ? b.w : nav;
        int menu_y = b.y + (narrow ? 64 : 88);
        if (narrow)
            rect(u, (SDL_Rect){b.x, menu_y, b.w, b.h - top}, u->surface);
        for (int i = 0; i < 9; i++) {
            int step = narrow ? SDL_min(42, (b.h - top) / 9) : 44;
            SDL_Rect row = {b.x + 10, menu_y + i * step, menu_width - 20, step - 4};
            if (u->section == i)
                rounded(u, row, 7, u->selected_bg);
            text(u, sections[i], row.x + 10, row.y + 8, row.w - 18, 12, u->section == i,
                 u->section == i ? u->ink : u->muted);
            hit(u, row, UI_SECTION + i, 0, sections[i]);
        }
        if (narrow) {
            close_button(u, b.x + b.w - 44, b.y + 15, PH_WORKSPACE_CLOSE);
            return;
        }
    }
    SDL_Rect clip = {x, b.y + top + 18, width, b.h - top - 30};
    SDL_RenderSetClipRect(u->renderer, &clip);
    text(u, sections[u->section], x, y, width - 35, 26, true, u->ink);
    y += 55;
    if (u->section == 0) {
        text(u, "Appearance", x, y, width, 12, true, u->muted);
        y += 27;
        const char *themes[] = {"Light", "Dark", "System"};
        for (int i = 0; i < 3; i++)
            button(u, (SDL_Rect){x + i * (width / 3), y, width / 3 - 8, 40}, themes[i],
                   UI_THEME + i, 0, u->theme == i, true);
        y += 66;
        y = setting(u, x, y, width, "Keep running in the background",
                    "Closing the window keeps connections and transfers available.", UI_TRAY,
                    u->tray);
        button(u, (SDL_Rect){x, y, 140, 34}, "Quit Porthole", PH_WORKSPACE_QUIT, 0, false, true);
        y += 50;
    } else if (u->section == 1) {
        y = setting(u, x, y, width, "PC audio", "Play Screen Mirroring audio on this PC.",
                    PH_WORKSPACE_SOUND, u->live.sound);
        char volume[48];
        snprintf(volume, sizeof volume, "Output volume · %d%%", u->live.volume);
        text(u, volume, x, y, width, 13, true, u->ink);
        y += 30;
        button(u, (SDL_Rect){x, y, 58, 32}, "−", PH_WORKSPACE_VOLUME_DOWN, 0, false, true);
        button(u, (SDL_Rect){x + 70, y, 58, 32}, "+", PH_WORKSPACE_VOLUME_UP, 0, false, true);
        y += 58;
        button(u, (SDL_Rect){x, y, 136, 34}, "Full screen", PH_WORKSPACE_FULLSCREEN, 0, false,
               u->live.mirroring);
        y += 50;
        button(u, (SDL_Rect){x, y, 136, 34}, "Save screenshot", PH_WORKSPACE_CAPTURE, 0, false,
               u->live.mirroring);
        y += 50;
        button(u, (SDL_Rect){x, y, 136, 34},
               u->live.recording ? "Stop recording" : "Start recording", PH_WORKSPACE_RECORD, 0,
               false, u->live.mirroring || u->live.recording);
        y += 50;
        button(u, (SDL_Rect){x, y, 158, 34}, "Open Captures folder", PH_WORKSPACE_CAPTURES, 0,
               false, true);
        y += 50;
    } else if (u->section == 2) {
        y = setting(u, x, y, width, "Enable PC controls",
                    "Use this PC’s mouse and keyboard during screen sharing.",
                    PH_WORKSPACE_CONTROLS, !u->live.paused);
        if (u->live.mirroring)
            y = setting(u, x, y, width, "Show control toolbar",
                        "Display navigation and volume controls beside your iPhone.",
                        PH_WORKSPACE_TOOLBAR, u->live.toolbar);
        y = setting(u, x, y, width, "Relative pointer",
                    "Restart Porthole and pair again after changing pointer mode.",
                    PH_WORKSPACE_POINTER, u->live.relative);
        y = setting(u, x, y, width, "Feed scrolling",
                    "Convert each wheel burst into a swipe. F4 toggles this mode.",
                    PH_WORKSPACE_FEED, u->live.feed);
        y = setting(u, x, y, width, "Use Ctrl as Command",
                    "Use Ctrl+C, Ctrl+V and Ctrl+A on your iPhone.", PH_WORKSPACE_CTRL_COMMAND,
                    u->live.ctrl_command);
        y = setting(u, x, y, width, "Forward media keys",
                    "Send playback and volume keys to your iPhone.", PH_WORKSPACE_MEDIA,
                    u->live.media_keys);
        y = setting(u, x, y, width, "Release input when switching apps",
                    "Release held keys and mouse buttons when Porthole loses focus.",
                    PH_WORKSPACE_RELEASE, u->live.release_blur);
        button(u, (SDL_Rect){x, y, 164, 34}, "Set up controls", PH_WORKSPACE_SETUP, 0, false, true);
        y += 50;
    } else if (u->section == 3) {
        text(u, "Receive folder", x, y, width, 13, true, u->ink);
        y += 28;
        text(u, u->live.folder, x, y, width, 12, false, u->muted);
        y += 30;
        button(u, (SDL_Rect){x, y, 140, 34}, "Change folder", PH_WORKSPACE_FOLDER, 0, false, true);
        y += 54;
        button(u, (SDL_Rect){x, y, 140, 34}, "File history", UI_HISTORY, 0, false, true);
        y += 54;
        button(u, (SDL_Rect){x, y, 140, 34}, "Clipboard history", PH_WORKSPACE_CLIPBOARD_OPEN, 0,
               false, true);
        y += 54;
        button(u, (SDL_Rect){x, y, SDL_min(width, 230), 34}, "Clear clipboard history on this PC",
               UI_CLEAR_ALL_CLIPBOARD, 0, false, true);
        y += 54;
        y = setting(u, x, y, width, "Clipboard sync",
                    "Synchronize text and images between your devices.", UI_SYNC, u->sync);
        y = setting(u, x, y, width, "PC to iPhone", "Send clipboard updates to your iPhone.",
                    UI_TO_PHONE, u->to_phone);
        y = setting(u, x, y, width, "iPhone to PC", "Receive clipboard updates from your iPhone.",
                    UI_TO_PC, u->to_pc);
    } else if (u->section == 4) {
        y = setting(u, x, y, width, "Transfer notifications",
                    "Notify me when files arrive while Porthole is minimized.", UI_NOTIFY,
                    u->notifications);
        y = text(u,
                 "Windows notification settings and Do not disturb can suppress alerts. Transfers "
                 "remain available in File history.",
                 x, y, width, 12, false, u->muted) +
            22;
    } else if (u->section == 5) {
        search_box(u, (SDL_Rect){x, y, width, 34});
        y += 48;
        button(u, (SDL_Rect){x, y, 112, 30}, "Guides", UI_GUIDES, 0, !u->shortcuts, true);
        button(u, (SDL_Rect){x + 124, y, 154, 30}, "Keyboard shortcuts", UI_SHORTCUTS, 0,
               u->shortcuts, true);
        y += 48;
        static const char *guides[] = {
            "Connect your iPhone\nOpen Porthole on your iPhone. Scan the QR code, or enter the "
            "address and connection code. Keep both devices on the same Wi-Fi network.",
            "Screen sharing\nSelect Share screen in Porthole on your iPhone, then Start Broadcast. "
            "File transfers and clipboard sync work without screen sharing.",
            "Mouse and keyboard\nEnable AssistiveTouch and disable Dwell Control on your iPhone. "
            "Pair the Porthole Bluetooth input device, then use Set up controls to check pointer "
            "alignment.",
            "Send and receive files\nDrop files into Porthole or select Send files. On iPhone, "
            "select files in Photos or Files, tap Share, then choose Porthole.",
            "Original files\nHold Ctrl and drag a photo or video from the shared screen to request "
            "its original. If matching fails, use Choose original in the iPhone app.",
            "Clipboard\nCopy text or an image to sync it. The Clipboard panel keeps up to 100 "
            "recent items on this PC. Copy, send, delete selected items, or clear history.",
            "File history\nView sent and received transfers. Show in folder selects the local "
            "file. Use Locate file if it has moved. Removing records keeps your files."};
        static const char *keys[] = {"F1   Help and shortcuts",
                                     "F2   Set up controls",
                                     "F3   Connection statistics",
                                     "F4   Toggle feed scrolling",
                                     "F8   Save screenshot",
                                     "F9   Start or stop recording",
                                     "Alt+Enter   Full screen",
                                     "Alt+H   Home",
                                     "Alt+S   App Switcher",
                                     "Alt+C   Control Center",
                                     "Alt+N   Notification Center",
                                     "Alt+P   iPhone screenshot",
                                     "Alt+Up / Alt+Down   iPhone volume",
                                     "Alt+M   Mute PC audio",
                                     "Ctrl+drag   Request original file",
                                     "F6   Switch connected screen",
                                     "Alt+T   Always on top",
                                     "Alt+1   Actual pixels",
                                     "Alt+F   Search on iPhone",
                                     "Alt+Left   Back",
                                     "Alt+Page Up / Page Down   Swipe",
                                     "Alt+Shift+Arrow   Directional swipe",
                                     "Alt+D   Double tap",
                                     "Alt+L   Touch and hold",
                                     "Alt+U   Swipe from the bottom",
                                     "Alt+K   Toggle iPhone keyboard",
                                     "Alt+- / =   Adjust PC volume",
                                     "Alt+, / .   Adjust audio sync",
                                     "Ctrl+Tab   Focus transfer controls during screen sharing",
                                     "Escape   Cancel gesture or close local panel"};
        int count = u->shortcuts ? (int)SDL_arraysize(keys) : (int)SDL_arraysize(guides);
        for (int i = 0; i < count; i++) {
            const char *s = u->shortcuts ? keys[i] : guides[i];
            if (!contains(s, u->search))
                continue;
            y = text(u, s, x, y, width, u->shortcuts ? 13 : 12, false, u->ink) + 24;
        }
    } else if (u->section == 6) {
        button(u, (SDL_Rect){x, y, 170, 34}, "Show statistics", PH_WORKSPACE_STATS, 0, false, true);
        y += 54;
    } else if (u->section == 8) {
        if (u->sessions.request_id[0]) {
            y = text(u, u->sessions.request_name, x, y, width, 17, true, u->ink) + 8;
            y = text(u, "Screen Mirroring access requested", x, y, width, 12, false, u->muted) + 12;
            button(u, (SDL_Rect){x, y, 108, 34}, "Review PIN", UI_REVIEW, 0, true, true);
            button(u, (SDL_Rect){x + 118, y, 90, 34}, "Reject", UI_REJECT, 0, false, true);
            y += 54;
        }
        bool registry = false;
        bool selected = false;
        for (int i = 0; i < u->sessions.count; i++) {
            if (u->sessions.devices[i].companion) {
                registry = true;
                selected = selected || u->sessions.devices[i].active;
            }
        }
        button(u, (SDL_Rect){x, y, 118, 32}, "Add iPhone", PH_WORKSPACE_ADD_PHONE, 0, true,
               u->live.paired);
        if (u->live.paired && (selected || !registry))
            button(u, (SDL_Rect){x + 128, y, 118, 32}, "New code", PH_WORKSPACE_NEW_CODE, 0,
                   false, true);
        y += 46;
        if (!registry && u->live.paired)
            y = device_row(u, x, y, width, 0);
        for (int i = 0; i < u->sessions.count; i++)
            y = device_row(u, x, y, width, i + 1);
        y = setting(u, x, y, width, "Require local approval",
                    "Verify a PIN on this PC before admitting a new Screen Mirroring device.",
                    PH_WORKSPACE_PIN_REQUIRED, u->sessions.pin_required);
    } else {
        y = text(u, "Porthole " PORTHOLE_VERSION "\nFree software · GPL-3.0", x, y, width, 15,
                 false, u->ink) +
            30;
    }
    SDL_RenderSetClipRect(u->renderer, NULL);
    close_button(u, b.x + b.w - 44, b.y + 15, PH_WORKSPACE_CLOSE);
    u->scroll_limit = SDL_max(y + u->scroll - b.y - b.h + 24, 0);
}
static void confirmation(ph_workspace *u)
{
    SDL_Rect b = u->rect;
    SDL_SetRenderDrawBlendMode(u->renderer, SDL_BLENDMODE_BLEND);
    rect(u, b, (SDL_Color){0, 0, 0, 110});
    SDL_SetRenderDrawBlendMode(u->renderer, SDL_BLENDMODE_NONE);
    int width = b.w > 500 ? 440 : b.w - 24;
    SDL_Rect box = {b.x + (b.w - width) / 2, b.y + (b.h - 210) / 2, width, 210};
    card(u, box);
    u->hits_n = 0;
    close_button(u, box.x + box.w - 42, box.y + 12, UI_CANCEL);
    if (u->confirm == 12) {
        text(u, "Clear all clipboard history?", box.x + 22, box.y + 25, box.w - 70, 20, true,
             u->ink);
        text(u, "Remove saved clipboard items for all iPhone pairings on this PC.", box.x + 22,
             box.y + 76, box.w - 44, 13, false, u->muted);
        button(u, (SDL_Rect){box.x + 22, box.y + 153, 172, 34}, "Clear clipboard history",
               UI_CONFIRM, 0, true, true);
        return;
    }
    if (u->confirm == 11) {
        text(u, "Certificate fingerprint", box.x + 20, box.y + 20, box.w - 70, 19, true, u->ink);
        text(u, "SHA-256", box.x + 20, box.y + 52, box.w - 40, 12, false, u->muted);
        const char *pin = ph_tls_fingerprint();
        char lines[80] = "";
        if (strlen(pin) == 64)
            snprintf(lines, sizeof lines, "%.16s\n%.16s\n%.16s\n%.16s", pin, pin + 16, pin + 32,
                     pin + 48);
        text(u, lines, box.x + 20, box.y + 74, box.w - 40, 12, true, u->ink);
        copy_button(u, (SDL_Rect){box.x + box.w - 56, box.y + 92, 36, 32}, UI_COPY_CODE, true);
        return;
    }
    if (u->confirm >= 7) {
        bool edit = u->confirm == 7 || u->confirm == 10;
        const char *title = u->confirm == 7   ? "Rename device"
                            : u->confirm == 8 ? "Disconnect device?"
                            : u->confirm == 9 ? "Forget device?"
                                              : "Confirm local PIN";
        text(u, title, box.x + 22, box.y + 22, box.w - 70, 21, true, u->ink);
        char description[160] = "";
        if (u->confirm == 10)
            snprintf(description, sizeof description, "Enter %s to approve this device.",
                     u->sessions.pin);
        else
            snprintf(description, sizeof description, "%s",
                     u->confirm == 7   ? "Device name"
                     : u->confirm == 8 ? "End this device’s current connection."
                                       : "Remove this device’s saved connection details.");
        text(u, description, box.x + 22, box.y + 68, box.w - 44, 13, false, u->muted);
        if (edit) {
            SDL_Rect field = {box.x + 22, box.y + 98, box.w - 44, 34};
            entry_box(u, field, u->edit_value, u->confirm == 10 ? "PIN" : "Device name",
                      UI_EDIT_DEVICE);
        }
        button(u, (SDL_Rect){box.x + 22, box.y + 153, 146, 34},
               u->confirm == 7   ? "Save name"
               : u->confirm == 8 ? "Disconnect"
               : u->confirm == 9 ? "Forget device"
                                 : "Confirm PIN",
               UI_CONFIRM, 0, true,
               !edit || (u->confirm == 10 ? strlen(u->edit_value) == 4 : u->edit_value[0] != 0));
        return;
    }
    const char *title = u->confirm == 6   ? "Restart this transfer?"
                        : u->confirm == 1 ? "Clear clipboard history?"
                        : u->confirm == 2 ? "Clear file history?"
                        : u->confirm == 3 ? "Remove this transfer?"
                        : u->confirm == 4 ? "Forget this device?"
                                          : "Replace connection code?";
    text(u, title, box.x + 22, box.y + 25, box.w - 70, 21, true, u->ink);
    const char *detail =
        u->confirm == 6  ? "The source file has changed. Send the current file from the beginning."
        : u->confirm < 4 ? "Remove the selected history records from this PC. Your clipboard and "
                           "files will be kept."
        : u->confirm == 4
            ? "Disconnect this device and require pairing again. Saved files will be kept."
            : "Disconnect the paired device and create a new connection code. Pair again to "
              "reconnect.";
    text(u, detail, box.x + 22, box.y + 69, box.w - 44, 13, false, u->muted);
    if (u->error[0])
        text(u, u->error, box.x + 22, box.y + 115, box.w - 44, 11, true, u->ink);
    button(u, (SDL_Rect){box.x + 22, box.y + 153, 164, 34},
           u->confirm == 6   ? "Restart transfer"
           : u->confirm < 4  ? "Remove history"
           : u->confirm == 4 ? "Forget device"
                             : "New code",
           UI_CONFIRM, 0, true, true);
}
static void draw_page(ph_workspace *u)
{
    SDL_Rect b = u->rect;
    ph_workspace_view view = u->view;
    u->hits_n = 0;
    u->scroll_limit = 0;
    rect(u, b,
         view == PH_WORKSPACE_CLIPBOARD || view == PH_WORKSPACE_FILES ? u->surface : u->background);
    if (view == PH_WORKSPACE_HOME)
        home(u);
    else if (view == PH_WORKSPACE_SETTINGS)
        settings(u);
    else if (view == PH_WORKSPACE_HISTORY)
        history_page(u);
    else if (view == PH_WORKSPACE_FILES)
        files_panel(u);
    else
        clipboard_page(u);
}

void ph_workspace_draw(ph_workspace *u, SDL_Renderer *renderer, SDL_Rect b, ph_workspace_view view,
                       const ph_workspace_live *live)
{
    if (!u || b.w < 1 || b.h < 1)
        return;
    if (view != u->view) {
        u->revision++;
        u->scroll = 0;
        u->pressed = (ui_hit){0};
        u->focused = (ui_hit){0};
        u->confirm = 0;
        u->error[0] = 0;
        if (u->editing) {
            SDL_StopTextInput();
            u->editing = false;
        }
    }
    u->renderer = renderer;
    u->rect = b;
    u->live = *live;
    u->view = view;
    palette(u);
    draw_page(u);
    if (u->scroll > u->scroll_limit) {
        u->scroll = u->scroll_limit;
        draw_page(u);
    }
    if (u->confirm)
        confirmation(u);
    else if (u->error[0]) {
        SDL_Rect banner = {b.x + 10, b.y + b.h - 66, b.w - 20, 56};
        card(u, banner);
        text(u, u->error, banner.x + 10, banner.y + 10, banner.w - 50, 12, true, u->ink);
        close_button(u, banner.x + banner.w - 34, banner.y + 8, UI_CANCEL);
    }
    int focused = focused_index(u);
    if (focused >= 0) {
        SDL_Rect box = u->hits[focused].box;
        color(u->renderer, ph_workspace_dark(u) ? (SDL_Color){144, 197, 250, 255}
                                               : (SDL_Color){32, 82, 171, 255});
        SDL_RenderDrawRect(u->renderer, &box);
        if (box.w > 2 && box.h > 2)
            SDL_RenderDrawRect(u->renderer, &(SDL_Rect){box.x + 1, box.y + 1, box.w - 2, box.h - 2});
    }
}

static void edit_start(ph_workspace *u, const char *value, bool replace)
{
    u->editing = true;
    u->cursor = strlen(value);
    u->anchor = replace ? 0 : u->cursor;
    SDL_StartTextInput();
}

static void edit_stop(ph_workspace *u)
{
    u->editing = false;
    u->cursor = u->anchor = 0;
    SDL_StopTextInput();
}

static void history_save_error(ph_workspace *u)
{
    snprintf(u->error, sizeof u->error, "Could not save history. Check storage and retry.");
}

static void activate(ph_workspace *u, ui_hit h)
{
    int a = h.action;
    bool had_error = u->error[0];
    u->error[0] = 0;
    if (a == UI_COPY_CODE && u->confirm == 11) {
        SDL_SetClipboardText(ph_tls_fingerprint());
        return;
    }
    if (a == UI_FINGERPRINT) {
        u->confirm = 11;
        return;
    }
    if (a == UI_CLEAR_ALL_CLIPBOARD) {
        u->confirm = 12;
        return;
    }
    if (a == UI_SECTION_MENU) {
        u->section_menu = !u->section_menu;
        return;
    }
    if (a >= UI_SECTION && a < UI_SECTION + 9) {
        ph_workspace_open_section(u, a - UI_SECTION);
        if (u->view == PH_WORKSPACE_HOME)
            queue(u, PH_WORKSPACE_SETTINGS_OPEN, 0);
        return;
    }
    if (a >= UI_THEME && a < UI_THEME + 3) {
        u->theme = a - UI_THEME;
        save_preferences(u);
        return;
    }
    if (a >= UI_FILTER && a < UI_FILTER + 3) {
        u->filter = a - UI_FILTER;
        u->scroll = 0;
        u->selected = 0;
        return;
    }
    if (a == UI_RENAME || a == UI_DISCONNECT || a == UI_FORGET || a == UI_SELECT_DEVICE ||
        a == UI_REVIEW || a == UI_REJECT) {
        if (h.id > (uint64_t)u->sessions.count)
            return;
        const ph_session_device *d = h.id ? &u->sessions.devices[h.id - 1] : NULL;
        snprintf(u->device_id, sizeof u->device_id, "%s",
                 a == UI_REVIEW || a == UI_REJECT ? u->sessions.request_id
                 : d                              ? d->id
                                                  : "");
        if (a == UI_SELECT_DEVICE) {
            queue(u, PH_WORKSPACE_DEVICE_SELECT, 0);
            return;
        }
        if (a == UI_REJECT) {
            queue(u, PH_WORKSPACE_PIN_REJECT, 0);
            return;
        }
        u->confirm = a == UI_RENAME ? 7 : a == UI_DISCONNECT ? 8 : a == UI_FORGET ? 9 : 10;
        snprintf(u->edit_value, sizeof u->edit_value, "%s",
                 a == UI_RENAME ? (d ? d->name : u->live.phone) : "");
        if (a == UI_RENAME || a == UI_REVIEW)
            edit_start(u, u->edit_value, a == UI_RENAME);
        return;
    }
    if (a == UI_EDIT_DEVICE) {
        edit_start(u, u->edit_value, false);
        return;
    }
    if (a == UI_SEARCH) {
        edit_start(u, u->search, false);
        return;
    }
    if (a == UI_GUIDES || a == UI_SHORTCUTS) {
        u->shortcuts = a == UI_SHORTCUTS;
        u->scroll = 0;
        return;
    }
    if (a == UI_DATE) {
        u->date_range = (u->date_range + 1) % 3;
        u->scroll = 0;
        u->selected = 0;
        return;
    }
    if (a == UI_HISTORY) {
        ph_workspace_open_history(u, false);
        queue(u, PH_WORKSPACE_HISTORY_OPEN, 0);
        return;
    }
    if (a == UI_SELECT) {
        ph_workspace_refresh_files(u);
        u->selected = h.id;
        if (u->view == PH_WORKSPACE_HOME || u->view == PH_WORKSPACE_FILES)
            queue(u, PH_WORKSPACE_HISTORY_OPEN, 0);
        return;
    }
    if (a == UI_CANCEL) {
        if (u->confirm) {
            u->confirm = 0;
            edit_stop(u);
        } else if (!had_error) {
            u->selected = 0;
        }
        return;
    }
    if (a == PH_WORKSPACE_COPY_ADDRESS || a == PH_WORKSPACE_COPY_CODE) {
        SDL_SetClipboardText(a == PH_WORKSPACE_COPY_ADDRESS ? u->live.address : u->live.code);
        return;
    }
    if (a == UI_CLEAR) {
        u->confirm = u->view == PH_WORKSPACE_CLIPBOARD ? 1 : 2;
        return;
    }
    if (a == UI_CLIP_CHECK) {
        if (!h.id) {
            bool all = u->clip_selected_n == ph_history_clip_count(u->history);
            u->clip_selected_n = 0;
            if (all)
                return;
            for (size_t i = 0; i < ph_history_clip_count(u->history); i++)
                u->clip_selected[u->clip_selected_n++] = ph_history_clip(u->history, i)->id;
        } else if (clip_checked(u, h.id)) {
            for (size_t i = 0; i < u->clip_selected_n; i++)
                if (u->clip_selected[i] == h.id) {
                    u->clip_selected[i] = u->clip_selected[--u->clip_selected_n];
                    break;
                }
        } else if (u->clip_selected_n < 100)
            u->clip_selected[u->clip_selected_n++] = h.id;
        return;
    }
    if (a == UI_CLIP_REMOVE) {
        if (u->view == PH_WORKSPACE_CLIPBOARD) {
            if (!ph_history_remove_clip(u->history, h.id))
                history_save_error(u);
        } else {
            u->confirm = 3;
        }
        return;
    }
    if (a == UI_CLIP_DELETE) {
        if (ph_history_remove_clips(u->history, u->clip_selected, u->clip_selected_n))
            u->clip_selected_n = 0;
        else
            history_save_error(u);
        return;
    }
    if (a == UI_CONFIRM && u->confirm == 12) {
        queue(u, PH_WORKSPACE_CLEAR_ALL_CLIPBOARD, 0);
        u->confirm = 0;
        return;
    }
    if (a == UI_CONFIRM && u->confirm >= 7) {
        queue(u,
              u->confirm == 7   ? PH_WORKSPACE_DEVICE_RENAME
              : u->confirm == 8 ? PH_WORKSPACE_DEVICE_DISCONNECT
              : u->confirm == 9 ? PH_WORKSPACE_DEVICE_FORGET
                                : PH_WORKSPACE_PIN_APPROVE,
              0);
        u->confirm = 0;
        u->editing = false;
        SDL_StopTextInput();
        return;
    }
    if (a == UI_CONFIRM) {
        if (u->confirm == 1) {
            if (!ph_history_clear_clips(u->history)) {
                history_save_error(u);
                return;
            }
            u->clip_selected_n = 0;
        } else if (u->confirm == 2) {
            if (!ph_history_clear_files(u->history)) {
                history_save_error(u);
                return;
            }
            u->selected = 0;
        } else if (u->confirm == 3) {
            if (!ph_history_remove_file(u->history, u->selected)) {
                history_save_error(u);
                return;
            }
            u->selected = 0;
        } else if (u->confirm == 6)
            queue(u, PH_WORKSPACE_RESTART, u->selected);
        else
            queue(u, u->confirm == 4 ? PH_WORKSPACE_FORGET : PH_WORKSPACE_NEW_CODE, 0);
        u->confirm = 0;
        return;
    }
    if (a == UI_TRAY || a == UI_NOTIFY || a == UI_SYNC || a == UI_TO_PHONE || a == UI_TO_PC) {
        bool *flag = a == UI_TRAY       ? &u->tray
                     : a == UI_NOTIFY   ? &u->notifications
                     : a == UI_SYNC     ? &u->sync
                     : a == UI_TO_PHONE ? &u->to_phone
                                        : &u->to_pc;
        *flag = !*flag;
        save_preferences(u);
        return;
    }
    if (a == PH_WORKSPACE_RESTART) {
        u->confirm = 6;
        return;
    }
    if (a == PH_WORKSPACE_FORGET) {
        u->confirm = 4;
        return;
    }
    if (a == PH_WORKSPACE_NEW_CODE && u->live.paired) {
        u->confirm = 5;
        return;
    }
    if (a == PH_WORKSPACE_CLOSE) {
        u->history_open = false;
        u->editing = false;
        SDL_StopTextInput();
    }
    queue(u, (ph_workspace_action)a, h.id);
}
static void edit_insert(ph_workspace *u, char *field, size_t capacity, const char *value)
{
    size_t length = strlen(value);
    size_t lo = SDL_min(u->anchor, u->cursor);
    size_t hi = SDL_max(u->anchor, u->cursor);
    if (!length || length >= capacity - (strlen(field) - hi + lo) ||
        strcspn(value, "\r\n\t") < length ||
        (u->confirm == 10 && strspn(value, "0123456789") != length))
        return;
    memmove(field + lo + length, field + hi, strlen(field + hi) + 1);
    memcpy(field + lo, value, length);
    u->cursor = u->anchor = lo + length;
    u->scroll = 0;
}

static void edit_key(ph_workspace *u, char *field, size_t capacity, const SDL_KeyboardEvent *key)
{
    SDL_Keycode symbol = key->keysym.sym;
    bool ctrl = (key->keysym.mod & KMOD_CTRL) != 0;
    bool shift = (key->keysym.mod & KMOD_SHIFT) != 0;
    size_t lo = SDL_min(u->anchor, u->cursor), hi = SDL_max(u->anchor, u->cursor);
    if (ctrl && symbol == SDLK_a) {
        u->anchor = 0;
        u->cursor = strlen(field);
    } else if (ctrl && symbol == SDLK_v) {
        char *copy = SDL_GetClipboardText();
        if (copy) {
            edit_insert(u, field, capacity, copy);
            SDL_free(copy);
        }
    } else if (ctrl && (symbol == SDLK_c || symbol == SDLK_x) && hi > lo && u->confirm != 10) {
        char selected[256];
        memcpy(selected, field + lo, hi - lo);
        selected[hi - lo] = 0;
        if (!SDL_SetClipboardText(selected) && symbol == SDLK_x) {
            memmove(field + lo, field + hi, strlen(field + hi) + 1);
            u->cursor = u->anchor = lo;
        }
    } else if (symbol == SDLK_BACKSPACE || symbol == SDLK_DELETE) {
        if (lo == hi) {
            if (symbol == SDLK_BACKSPACE)
                lo = previous_char(field, lo);
            else
                hi = next_char(field, hi);
        }
        memmove(field + lo, field + hi, strlen(field + hi) + 1);
        u->cursor = u->anchor = lo;
        u->scroll = 0;
    } else if (symbol == SDLK_LEFT || symbol == SDLK_RIGHT || symbol == SDLK_HOME ||
               symbol == SDLK_END) {
        if (symbol == SDLK_HOME)
            u->cursor = 0;
        else if (symbol == SDLK_END)
            u->cursor = strlen(field);
        else if (symbol == SDLK_LEFT)
            u->cursor = !shift && lo != hi ? lo : previous_char(field, u->cursor);
        else
            u->cursor = !shift && lo != hi ? hi : next_char(field, u->cursor);
        if (!shift)
            u->anchor = u->cursor;
    } else if (symbol == SDLK_RETURN || symbol == SDLK_KP_ENTER) {
        for (int i = 0; u->confirm && i < u->hits_n; i++)
            if (u->hits[i].action == UI_CONFIRM) {
                activate(u, u->hits[i]);
                return;
            }
        edit_stop(u);
    } else if (symbol == SDLK_ESCAPE) {
        if (u->confirm)
            u->confirm = 0;
        edit_stop(u);
    }
}

bool ph_workspace_event(ph_workspace *u, const SDL_Event *event)
{
    if (!u)
        return false;
    bool keyboard = event->type == SDL_KEYDOWN || event->type == SDL_KEYUP;
    SDL_Keycode symbol = keyboard ? event->key.keysym.sym : SDLK_UNKNOWN;
    if (keyboard && symbol == SDLK_UNKNOWN)
        symbol = SDL_GetKeyFromScancode(event->key.keysym.scancode);
    if (u->editing && event->type == SDL_TEXTINPUT) {
        char *field = u->confirm ? u->edit_value : u->search;
        size_t capacity = u->confirm == 10 ? 5 : u->confirm ? sizeof u->edit_value : sizeof u->search;
        edit_insert(u, field, capacity, event->text.text);
        return true;
    }
    if (keyboard && symbol == SDLK_TAB && u->hits_n &&
        (!u->live.mirroring || u->live.modal || u->focused.action ||
         (event->key.keysym.mod & KMOD_CTRL))) {
        if (event->type == SDL_KEYDOWN) {
            if (u->editing)
                edit_stop(u);
            int index = focused_index(u);
            int step = (event->key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
            if (index < 0)
                index = step == 1 ? 0 : u->hits_n - 1;
            else
                index = (index + step + u->hits_n) % u->hits_n;
            u->focused = u->hits[index];
        }
        return true;
    }
    if (u->editing && keyboard) {
        char *field = u->confirm ? u->edit_value : u->search;
        size_t capacity = u->confirm == 10 ? 5 : u->confirm ? sizeof u->edit_value : sizeof u->search;
        if (event->type == SDL_KEYDOWN)
            edit_key(u, field, capacity, &event->key);
        return true;
    }
    if (event->type == SDL_KEYDOWN && symbol == SDLK_ESCAPE &&
        (u->confirm || u->history_open || u->view == PH_WORKSPACE_SETTINGS ||
         u->view == PH_WORKSPACE_CLIPBOARD || u->focused.action)) {
        if (u->confirm)
            u->confirm = 0;
        else if (u->view != PH_WORKSPACE_HOME) {
            u->history_open = false;
            queue(u, PH_WORKSPACE_CLOSE, 0);
        }
        u->focused = (ui_hit){0};
        return true;
    }
    if (keyboard && (symbol == SDLK_RETURN || symbol == SDLK_KP_ENTER || symbol == SDLK_SPACE) &&
        u->focused.action) {
        int index = focused_index(u);
        if (event->type == SDL_KEYDOWN && !event->key.repeat && index >= 0)
            activate(u, u->hits[index]);
        return true;
    }
    if (event->type == SDL_KEYDOWN && !u->confirm && (!u->live.mirroring || u->live.modal ||
                                                     u->focused.action)) {
        if (symbol == SDLK_PAGEUP || symbol == SDLK_PAGEDOWN) {
            int direction = symbol == SDLK_PAGEUP ? -1 : 1;
            u->scroll = SDL_clamp(u->scroll + direction * 230, 0, u->scroll_limit);
            return true;
        }
        if (symbol == SDLK_HOME || symbol == SDLK_END) {
            u->scroll = symbol == SDLK_HOME ? 0 : u->scroll_limit;
            return true;
        }
    }
    int x = 0, y = 0;
    if (event->type == SDL_MOUSEWHEEL) {
        SDL_GetMouseState(&x, &y);
        if (!SDL_PointInRect(&(SDL_Point){x, y}, &u->rect))
            return false;
        int amount = SDL_clamp(event->wheel.y, -20, 20);
        if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            amount = -amount;
        if (!u->confirm)
            u->scroll = SDL_clamp(u->scroll - amount * 42, 0, u->scroll_limit);
        return true;
    }
    if (event->type == SDL_MOUSEMOTION) {
        x = event->motion.x;
        y = event->motion.y;
    } else if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) {
        x = event->button.x;
        y = event->button.y;
    } else {
        return false;
    }
    bool inside = SDL_PointInRect(&(SDL_Point){x, y}, &u->rect);
    if (!inside && event->type == SDL_MOUSEBUTTONDOWN) {
        u->focused = (ui_hit){0};
        if (u->editing)
            edit_stop(u);
    }
    if (!inside && event->type == SDL_MOUSEBUTTONUP) {
        bool pressed = u->pressed.action != 0;
        u->pressed = (ui_hit){0};
        return pressed;
    }
    if (!inside)
        return false;
    if (event->type != SDL_MOUSEMOTION && event->button.button != SDL_BUTTON_LEFT)
        return true;
    for (int i = u->hits_n - 1; event->type != SDL_MOUSEMOTION && i >= 0; i--) {
        const ui_hit *target = &u->hits[i];
        if (!SDL_PointInRect(&(SDL_Point){x, y}, &target->box))
            continue;
        if (event->type == SDL_MOUSEBUTTONDOWN) {
            if (u->editing && target->action != UI_SEARCH && target->action != UI_EDIT_DEVICE)
                edit_stop(u);
            u->pressed = *target;
        } else {
            if (same_hit(&u->pressed, target) && SDL_RectEquals(&u->pressed.box, &target->box))
                activate(u, *target);
            u->pressed = (ui_hit){0};
        }
        return true;
    }
    if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP)
        u->pressed = (ui_hit){0};
    return true;
}

bool ph_workspace_owns_keyboard(const ph_workspace *u)
{
    return u && (u->editing || u->confirm || u->focused.action);
}

#ifdef _WIN32
int ph_workspace_accessible_count(const ph_workspace *u)
{
    return u ? u->hits_n : 0;
}

bool ph_workspace_accessible_control(const ph_workspace *u, int index, ph_workspace_control *out)
{
    if (!u || index < 0 || index >= u->hits_n)
        return false;
    const ui_hit *h = &u->hits[index];
    *out = (ph_workspace_control){.bounds = h->box, .action = h->action, .item = h->id,
                                  .revision = h->revision, .focused = focused_index(u) == index};
    snprintf(out->name, sizeof out->name, "%s", h->name);
    out->editable = h->action == UI_SEARCH || h->action == UI_EDIT_DEVICE;
    out->sensitive = h->action == UI_EDIT_DEVICE && u->confirm == 10;
    if (out->editable && !out->sensitive)
        snprintf(out->value, sizeof out->value, "%s",
                 h->action == UI_SEARCH ? u->search : u->edit_value);
    size_t length = strlen(h->name);
    const char *suffix = length > 6 ? h->name + length - 6 : h->name;
    bool on = length >= 4 && !strcmp(h->name + length - 4, "(on)");
    out->toggle = on || !strcmp(suffix, " (off)") || (h->action == UI_CLIP_CHECK && h->id);
    out->checked = on || (h->action == UI_CLIP_CHECK && h->id && clip_checked(u, h->id));
    if (h->action == PH_WORKSPACE_COPY_ADDRESS)
        snprintf(out->value, sizeof out->value, "%s", u->live.address);
    else if (h->action == PH_WORKSPACE_COPY_CODE)
        snprintf(out->value, sizeof out->value, "%s", u->live.code);
    return true;
}

bool ph_workspace_accessible_invoke(ph_workspace *u, const ph_workspace_control *control,
                                    bool focus_only, const char *value)
{
    if (!u || !control)
        return false;
    for (int i = 0; i < u->hits_n; i++) {
        const ui_hit *h = &u->hits[i];
        if (h->action != control->action || h->id != control->item ||
            h->revision != control->revision || strcmp(h->name, control->name))
            continue;
        u->focused = *h;
        if (value && control->editable && !control->sensitive) {
            char *field = h->action == UI_SEARCH ? u->search : u->edit_value;
            size_t capacity = h->action == UI_SEARCH ? sizeof u->search : sizeof u->edit_value;
            edit_start(u, field, true);
            if (!value[0])
                field[0] = 0;
            else
                edit_insert(u, field, capacity, value);
            if (!field[0])
                u->cursor = u->anchor = 0;
        } else if (!focus_only) {
            activate(u, *h);
        } else if (u->editing) {
            edit_stop(u);
        }
        return true;
    }
    return false;
}
#endif

ph_workspace_action ph_workspace_take_action(ph_workspace *u, uint64_t *id)
{
    if (!u)
        return PH_WORKSPACE_NONE;
    ph_workspace_action a = u->action;
    u->action = PH_WORKSPACE_NONE;
    if (id)
        *id = u->action_id;
    return a;
}
bool ph_workspace_target(ph_workspace *u, ph_workspace_action action, int *x, int *y)
{
    if (!u)
        return false;
    int key = action == PH_WORKSPACE_HISTORY_OPEN  ? UI_HISTORY
              : action == PH_WORKSPACE_DEVICE_SELECT ? UI_SELECT_DEVICE
                                                     : (int)action;
    for (int i = 0; i < u->hits_n; i++)
        if (u->hits[i].action == key) {
            *x = u->hits[i].box.x + u->hits[i].box.w / 2;
            *y = u->hits[i].box.y + u->hits[i].box.h / 2;
            return true;
        }
    return false;
}
