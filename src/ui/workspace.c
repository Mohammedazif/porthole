#include "ui/workspace.h"
#include "ui/clipboard_preview.h"
#include "ui/control_icons.h"
#include "ui/drawing.h"
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
#include "ui/settings_help.h"

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
    UI_CLEAR_ALL_CLIPBOARD,
    UI_HELP_GUIDE,
    UI_SIGN_IN,
    UI_KEEP_ASPECT
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
    bool section_menu, date_menu;
    int date_range;
    struct {
        uint64_t id;
        ph_file_availability value;
    } availability[64];
    uint64_t selected, clip_selected[100];
    size_t clip_selected_n;
    struct {
        uint64_t id;
        SDL_Texture *texture;
        int width, height;
    } previews[24];
    bool history_open, editing, tray, notifications, sync, to_phone, to_pc, shortcuts;
    bool launch_at_sign_in, launch_managed, keep_aspect;
    unsigned expanded_guides;
    char search[256];
    char error[128];
    ph_workspace_action action;
    uint64_t action_id;
    atomic_bool system_dark, stop;
    pthread_t theme_thread;
    bool theme_started;
    SDL_Color ink, muted, surface, background, border, selected_bg, positive;
};
static const char *pc_name(const ph_workspace *u)
{
    return u->live.pc[0] ? u->live.pc : "Porthole";
}

static void clear_previews(ph_workspace *u)
{
    for (size_t i = 0; i < SDL_arraysize(u->previews); i++)
        if (u->previews[i].texture)
            SDL_DestroyTexture(u->previews[i].texture);
    memset(u->previews, 0, sizeof u->previews);
}
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
    ph_ui_round_rect(u->renderer, b, radius, c);
}
static int text(ph_workspace *u, const char *s, int x, int y, int width, int size, bool bold,
                SDL_Color c)
{
    return ph_text_draw(u->renderer, s, x, y, width,
                        PH_TEXT(size, bold ? PH_FONT_MEDIUM : PH_FONT_REGULAR), c);
}
static void fitted_text(ph_workspace *u, const char *s, int x, int y, int width, int font,
                        SDL_Color c)
{
    if (width < 12)
        return;
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
    ph_text_draw(u->renderer, buf, x, y, width, font, c);
}
static void fitted_label(ph_workspace *u, const char *s, int x, int y, int width, int size,
                         bool bold, SDL_Color c)
{
    fitted_text(u, s, x, y, width, PH_TEXT(size, bold ? PH_FONT_MEDIUM : PH_FONT_REGULAR), c);
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

static void action_button(ph_workspace *u, SDL_Rect b, const char *label, int action, uint64_t id,
                          bool primary, bool enabled, bool framed, int icon)
{
    SDL_Color bg = primary ? u->ink : u->surface, fg = primary ? u->surface : u->ink;
    if (!enabled) {
        bg = u->selected_bg;
        fg = u->muted;
    }
    if (framed) {
        if (primary && enabled)
            rounded(u, b, 7, bg);
        else {
            rounded(u, b, 7, u->border);
            rounded(u, (SDL_Rect){b.x + 1, b.y + 1, b.w - 2, b.h - 2}, 6, bg);
        }
    } else
        fg = u->muted;
    int icon_space = icon < 0 ? 0 : 22;
    int font = PH_TEXT(12, PH_FONT_MEDIUM), tw = ph_text_width(label, font) + icon_space;
    int padding = SDL_max(4, (b.w - tw) / 2);
    if (icon >= 0)
        ph_icon_draw(u->renderer, (ph_control_icon)icon,
                     (SDL_Rect){b.x + padding, b.y + (b.h - 15) / 2, 15, 15}, fg);
    fitted_text(u, label, b.x + padding + icon_space, b.y + (b.h - ph_text_line_height(font)) / 2,
                b.w - padding - icon_space - 4, font, fg);
    if (enabled)
        hit(u, b, action, id, label);
}
static void button(ph_workspace *u, SDL_Rect b, const char *label, int action, uint64_t id,
                   bool primary, bool enabled)
{
    action_button(u, b, label, action, id, primary, enabled, true, -1);
}
static void copy_button(ph_workspace *u, SDL_Rect b, int action, bool enabled)
{
    ph_icon_draw(u->renderer, PH_ICON_COPY,
                 (SDL_Rect){b.x + (b.w - 18) / 2, b.y + (b.h - 18) / 2, 18, 18},
                 u->muted);
    if (enabled)
        hit(u, b, action, 0, action == PH_WORKSPACE_COPY_ADDRESS ? "Copy address"
                            : action == PH_WORKSPACE_COPY_CODE ? "Copy connection code"
                                                              : "Copy certificate fingerprint");
}
static void close_button(ph_workspace *u, int x, int y, int action)
{
    SDL_Rect b = {x, y, 28, 28};
    ph_icon_draw(u->renderer, PH_ICON_CLOSE, (SDL_Rect){x + 5, y + 5, 18, 18}, u->muted);
    hit(u, b, action, 0, action == PH_WORKSPACE_CANCEL_SEND ? "Cancel send" : "Close");
}
static void status_dot(ph_workspace *u, int x, int y, bool good)
{
    rounded(u, (SDL_Rect){x, y, 6, 6}, 3, good ? u->positive : u->muted);
}
static void toggle(ph_workspace *u, int x, int y, const char *name, bool on, int action,
                   bool enabled)
{
    SDL_Color off = ph_workspace_dark(u) ? (SDL_Color){83, 96, 115, 255}
                                         : (SDL_Color){185, 195, 207, 255};
    SDL_Color knob = on ? u->surface : (SDL_Color){255, 255, 255, 255};
    rounded(u, (SDL_Rect){x, y, 36, 21}, 10, on ? u->ink : off);
    rounded(u, (SDL_Rect){x + (on ? 17 : 2), y + 2, 17, 17}, 9, knob);
    if (enabled) {
        char label[96];
        snprintf(label, sizeof label, "%s (%s)", name, on ? "on" : "off");
        hit(u, (SDL_Rect){x - 8, y - 8, 52, 36}, action, 0, label);
    }
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
    u->ink = dark ? (SDL_Color){238, 241, 246, 255} : (SDL_Color){21, 26, 33, 255};
    u->muted = dark ? (SDL_Color){160, 171, 187, 255} : (SDL_Color){102, 114, 130, 255};
    u->surface = dark ? (SDL_Color){30, 34, 40, 255} : (SDL_Color){255, 255, 255, 255};
    u->background = dark ? (SDL_Color){21, 24, 28, 255} : (SDL_Color){243, 244, 246, 255};
    u->border = dark ? (SDL_Color){55, 62, 73, 255} : (SDL_Color){220, 226, 234, 255};
    u->selected_bg = dark ? (SDL_Color){52, 60, 72, 255} : (SDL_Color){233, 237, 242, 255};
    u->positive = dark ? (SDL_Color){91, 207, 152, 255} : (SDL_Color){22, 128, 71, 255};
}
static void save_preferences(ph_workspace *u)
{
    char value[96];
    snprintf(value, sizeof value, "%d %d %d %d %d %d %d", u->theme, u->tray, u->notifications,
             u->sync, u->to_phone, u->to_pc, u->keep_aspect);
    ph_config_write_line("desktop-preferences", value);
}
ph_workspace *ph_workspace_create(void)
{
    ph_workspace *u = calloc(1, sizeof *u);
    char value[96];
    int theme = 2, tray = 1, notify = 1, sync = 1, to = 1, from = 1, keep_aspect = 1;
    if (!u)
        return NULL;
    if (ph_config_read_line("desktop-preferences", value, sizeof value))
        if (sscanf(value, "%d %d %d %d %d %d %d",
                   &theme, &tray, &notify, &sync, &to, &from, &keep_aspect) < 6)
            theme = 2;
    u->theme = theme >= 0 && theme <= 2 ? theme : 2;
    u->tray = tray != 0;
    u->notifications = notify != 0;
    u->sync = sync != 0;
    u->to_phone = to != 0;
    u->to_pc = from != 0;
    u->keep_aspect = keep_aspect != 0;
    u->launch_at_sign_in = ph_config_launch_at_sign_in();
    u->launch_managed = ph_config_launch_managed();
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
        clear_previews(u);
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
        clear_previews(u);
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
bool ph_workspace_keep_aspect(const ph_workspace *u)
{
    return !u || u->keep_aspect;
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
        u->focused = (ui_hit){0};
        u->expanded_guides = 0;
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
    u->date_menu = false;
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
    else if (bytes < 1024)
        snprintf(out, 40, "%u B", (unsigned)bytes);
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
    float sx, sy;
    SDL_RenderGetScale(u->renderer, &sx, &sy);
    int left = (int)lroundf(box.x * sx), top = (int)lroundf(box.y * sy);
    int width = (int)lroundf((box.x + box.w) * sx) - left;
    int height = (int)lroundf((box.y + box.h) * sy) - top;
    int n = qrcodegen_getSize(code), scale = SDL_min(width, height) / (n + 8);
    if (scale < 1)
        return;
    int x = left + (width - scale * n) / 2, y = top + (height - scale * n) / 2;
    rounded(u, box, 0, (SDL_Color){255, 255, 255, 255});
    color(u->renderer, (SDL_Color){17, 17, 17, 255});
    SDL_FRect modules[64];
    int count = 0;
    for (int cy = 0; cy < n; cy++)
        for (int cx = 0; cx < n; cx++)
            if (qrcodegen_getModule(code, cx, cy)) {
                modules[count++] = ph_ui_pixel_rect(x + cx * scale, y + cy * scale,
                                                    scale, scale, sx, sy);
                if (count == (int)SDL_arraysize(modules)) {
                    SDL_RenderFillRectsF(u->renderer, modules, count);
                    count = 0;
                }
            }
    if (count)
        SDL_RenderFillRectsF(u->renderer, modules, count);
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
    bool search = action == UI_SEARCH;
    if (search) {
        rounded(u, box, 8, u->border);
        rounded(u, (SDL_Rect){box.x + 1, box.y + 1, box.w - 2, box.h - 2}, 7, u->surface);
        ph_icon_draw(u->renderer, PH_ICON_SEARCH,
                     (SDL_Rect){box.x + 12, box.y + 11, 16, 16}, u->muted);
    } else {
        rounded(u, box, 7, u->selected_bg);
    }
    int x = box.x + (search ? 39 : 10);
    int width = box.x + box.w - x - 10;
    int label_y = box.y + (box.h - ph_text_line_height(PH_TEXT(12, PH_FONT_REGULAR))) / 2;
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
        fitted_label(u, value + start, x, label_y, width, 12, false, u->ink);
        int at = x + text_span_width(value + start, cursor - start, font);
        rect(u, (SDL_Rect){at, box.y + 7, 1, box.h - 14}, u->ink);
        SDL_SetTextInputRect(&box);
    } else {
        fitted_label(u, value[0] ? value : label, x, label_y, width, 12, false,
                     value[0] ? u->ink : u->muted);
    }
    hit(u, box, action, 0, label);
}

static void search_box(ph_workspace *u, SDL_Rect box)
{
    entry_box(u, box, u->search,
              u->view == PH_WORKSPACE_HISTORY ? "Search files or locations" : "Search", UI_SEARCH);
}

static void file_badge(ph_workspace *u, SDL_Rect box, const char *name)
{
    const char *extension = strrchr(name, '.');
    size_t length = extension ? strlen(++extension) : 0;
    char type[6];
    if (length > 0 && length < sizeof type) {
        for (size_t i = 0; i < length; i++) {
            if (!SDL_isalnum((unsigned char)extension[i])) {
                length = 0;
                break;
            }
            type[i] = (char)SDL_toupper((unsigned char)extension[i]);
        }
    } else
        length = 0;
    type[length] = 0;
    rounded(u, box, 5, u->border);
    rounded(u, (SDL_Rect){box.x + 1, box.y + 1, box.w - 2, box.h - 2}, 4, u->surface);
    if (!length) {
        ph_icon_draw(u->renderer, PH_ICON_FILE,
                     (SDL_Rect){box.x + 7, box.y + 10, 20, 20}, u->muted);
        return;
    }
    SDL_Color fill = !strcmp(type, "PDF") ? (SDL_Color){153, 52, 48, 255}
                                         : u->selected_bg;
    SDL_Color ink = !strcmp(type, "PDF") ? (SDL_Color){255, 255, 255, 255}
                                        : u->ink;
    int points = 9;
    while (points > 6 && ph_text_width(type, PH_TEXT(points, PH_FONT_SEMIBOLD)) > box.w - 8)
        points--;
    int font = PH_TEXT(points, PH_FONT_SEMIBOLD);
    int width = ph_text_width(type, font);
    rounded(u, (SDL_Rect){box.x + 3, box.y + box.h - 18, box.w - 6, 13}, 2, fill);
    ph_text_draw(u->renderer, type, box.x + (box.w - width) / 2, box.y + box.h - 17,
                 box.w - 4, font, ink);
}

static time_t history_cutoff(const ph_workspace *u)
{
    if (!u->date_range)
        return 0;
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    if (!local)
        return 0;
    struct tm day = *local;
    day.tm_hour = day.tm_min = day.tm_sec = 0;
    day.tm_isdst = -1;
    if (u->date_range == 2)
        day.tm_mday -= 6;
    return mktime(&day);
}

static bool file_matches(const ph_workspace *u, const ph_file_entry *f, time_t cutoff)
{
    return f->time >= cutoff && (u->filter != 1 || !f->incoming) &&
           (u->filter != 2 || f->incoming) &&
           (contains(f->name, u->search) || contains(f->path, u->search));
}

static int history_row_count(const ph_workspace *u, int limit)
{
    time_t cutoff = history_cutoff(u);
    int count = 0;
    for (size_t i = 0; i < ph_history_file_count(u->history) && count < limit; i++)
        if (file_matches(u, ph_history_file(u->history, i), cutoff))
            count++;
    return count;
}

static void history_empty(ph_workspace *u, SDL_Rect rows, ph_control_icon icon,
                          const char *heading, const char *hint)
{
    if (rows.h < 66)
        return;
    int x = rows.x + 12, y = rows.y;
    rounded(u, (SDL_Rect){x, y + 17, 38, 38}, 9, u->selected_bg);
    ph_icon_draw(u->renderer, icon, (SDL_Rect){x + 9, y + 26, 20, 20}, u->muted);
    fitted_label(u, heading, x + 52, y + 15, rows.w - 76, 13, true, u->ink);
    text(u, hint, x + 52, y + 37, rows.w - 76, 11, false, u->muted);
}

static SDL_Rect history_header(ph_workspace *u, const char *title, const char *subtitle,
                               bool panel)
{
    SDL_Rect b = u->rect;
    int pad = panel ? 12 : b.w < 600 ? 20 : 32;
    b.x += pad;
    b.w -= 2 * pad;
    close_button(u, b.x + b.w - 28, b.y + (panel ? 9 : 24), PH_WORKSPACE_CLOSE);
    text(u, title, b.x, b.y + (panel ? 12 : 28), b.w - 40, panel ? 18 : 27, true, u->ink);
    if (panel)
        fitted_label(u, subtitle, b.x, b.y + 40, b.w, 12, false, u->muted);
    else
        text(u, subtitle, b.x, b.y + 68, b.w, 12, false, u->muted);
    return b;
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
    time_t cutoff = full ? history_cutoff(u) : 0;
    for (size_t i = 0; i < ph_history_file_count(u->history); i++) {
        const ph_file_entry *f = ph_history_file(u->history, i);
        if (!file_matches(u, f, cutoff))
            continue;
        any = true;
        if (cy + 66 > y && cy < y + clip.h) {
            SDL_Rect row = {b.x, cy, b.w, 66};
            if (u->selected == f->id)
                rounded(u, row, 6, u->selected_bg);
            char when[40], size[40], sub[128];
            timestamp(f->time, when);
            size_text(f->bytes, size);
            snprintf(sub, sizeof sub, "%s · %s", size, f->incoming ? "From iPhone" : "To iPhone");
            int right = b.w > 420 ? 120 : 86;
            file_badge(u, (SDL_Rect){b.x + 2, cy + 13, 34, 40}, f->name);
            fitted_label(u, f->name, b.x + 50, cy + 12, b.w - right - 58, 13, true, u->ink);
            ph_icon_draw(u->renderer, f->incoming ? PH_ICON_RECEIVE : PH_ICON_SEND,
                         (SDL_Rect){b.x + 50, cy + 35, 13, 13}, u->muted);
            fitted_label(u, sub, b.x + 68, cy + 35, b.w - right - 76, 10, false, u->muted);
            const char *state = f->state == PH_TRANSFER_COMPLETE
                                    ? (f->incoming ? "Received" : "Sent")
                                : f->state == PH_TRANSFER_CHANGED  ? "Source changed"
                                : f->state == PH_TRANSFER_FAILED   ? "Failed"
                                : f->state == PH_TRANSFER_CANCELED ? "Canceled"
                                : f->state == PH_TRANSFER_PAUSED   ? "Paused"
                                : f->state == PH_TRANSFER_ACTIVE   ? "Transferring"
                                                                   : "Queued";
            ph_file_availability state_on_disk = availability(u, f);
            const char *detail = state_on_disk == PH_FILE_MISSING ? "File not found"
                                 : state_on_disk == PH_FILE_LOCATION_UNAVAILABLE
                                     ? "Location unavailable" : when;
            int edge = b.x + b.w - 10;
            int state_width = ph_text_width(state, PH_TEXT(11, PH_FONT_REGULAR));
            int detail_width = ph_text_width(detail, PH_TEXT(10, PH_FONT_REGULAR));
            fitted_label(u, state, edge - SDL_min(state_width, right - 10), cy + 12,
                         right - 10, 11, false,
                         f->state == PH_TRANSFER_COMPLETE ? u->positive : u->muted);
            fitted_label(u, detail, edge - SDL_min(detail_width, right - 10), cy + 35,
                         right - 10, 10, false, u->muted);
            hit(u, row, UI_SELECT, f->id, f->name);
            line(u, b.x + 12, cy + 65, b.w - 24);
        }
        cy += 66;
    }
    SDL_RenderSetClipRect(u->renderer, NULL);
    if (!any) {
        const char *heading = u->search[0] || (full && u->date_range) ? "No matching transfers"
                              : u->filter == 2 ? "No received transfers"
                              : u->filter == 1 ? "No sent transfers" : "No file transfers yet";
        const char *hint = u->search[0] || (full && u->date_range)
                               ? "Try a different search or date range."
                           : u->filter == 2 ? "Files received from your iPhone appear here."
                           : u->filter == 1 ? "Files sent to your iPhone appear here."
                           : "Sent and received files appear here.";
        history_empty(u, clip, u->filter == 2 ? PH_ICON_RECEIVE : PH_ICON_FILE, heading, hint);
    }
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
    b.h = SDL_min(b.h, 370);
    card(u, b);
    int x = b.x + 18, y = b.y + 18, width = b.w - 36;
    SDL_Rect heading_clip = {x, y, width - 26, 48};
    SDL_RenderSetClipRect(u->renderer, &heading_clip);
    int title_end = text(u, f->name, x, y, width - 26, 16, true, u->ink);
    SDL_RenderSetClipRect(u->renderer, NULL);
    y = SDL_min(title_end, y + 48) + 7;
    close_button(u, b.x + b.w - 38, b.y + 10, UI_CANCEL);
    char date[40];
    timestamp(f->time, date);
    y = text(u, date, x, y, width, 11, false, u->muted) + 24;
    y = text(u, f->incoming ? "SAVED LOCATION" : "SOURCE LOCATION", x, y, width, 10, true,
             u->muted) +
        9;
    SDL_Rect location_clip = {x, y, width, 36};
    SDL_RenderSetClipRect(u->renderer, &location_clip);
    int path_end = text(u, f->path[0] ? f->path : "Location not recorded",
                        x, y, width, 11, false, u->ink);
    SDL_RenderSetClipRect(u->renderer, NULL);
    y = SDL_min(path_end, y + 36) + 14;
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
    action_button(u, (SDL_Rect){x, y, SDL_min(width, 160), 34},
                  can_locate ? "Locate file…" : "Show in folder",
                  can_locate ? PH_WORKSPACE_LOCATE : PH_WORKSPACE_REVEAL, f->id, false,
                  can_locate || state == PH_FILE_PRESENT, true, PH_ICON_FOLDER);
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
    const char *dates[] = {"All time", "Today", "Last 7 days"};
    SDL_Rect b = history_header(u, "File history",
                               "All recorded transfers for this iPhone, including previous days.",
                               false);
    search_box(u, (SDL_Rect){b.x, b.y + 104, b.w - 130, 36});
    SDL_Rect date = {b.x + b.w - 118, b.y + 104, 118, 36};
    button(u, date, dates[u->date_range], UI_DATE, 0, false, true);
    ph_icon_draw(u->renderer, PH_ICON_DOWN,
                 (SDL_Rect){date.x + date.w - 22, date.y + 11, 14, 14}, u->muted);
    int detail = u->selected && u->rect.w >= 850 ? 340 : 0;
    int remaining = b.h - 184;
    int matches = history_row_count(u, SDL_max(1, remaining / 66));
    int list_height = SDL_min(remaining, matches ? 64 + matches * 66 : 157);
    SDL_Rect list = {b.x, b.y + 160, b.w - detail - (detail ? 20 : 0), list_height};
    if (!u->selected || detail) {
        card(u, list);
        SDL_Rect rows = {list.x + 14, list.y + 10, list.w - 28, list.h - 20};
        draw_file_rows(u, rows, true);
    }
    if (detail)
        draw_file_detail(u, (SDL_Rect){list.x + list.w + 20, list.y, detail, remaining});
    else if (u->selected)
        draw_file_detail(u, (SDL_Rect){b.x, b.y + 160, b.w, remaining});
    if (u->date_menu) {
        SDL_Rect menu = {date.x, date.y + date.h + 5, date.w, 110};
        card(u, menu);
        for (size_t i = 0; i < SDL_arraysize(dates); i++) {
            SDL_Rect option = {menu.x + 5, menu.y + 5 + (int)i * 33, menu.w - 10, 32};
            if ((int)i == u->date_range)
                rounded(u, option, 6, u->selected_bg);
            text(u, dates[i], option.x + 10, option.y + 8, option.w - 20, 12, false, u->ink);
            hit(u, option, UI_DATE, i + 1, dates[i]);
        }
    }
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

static void image_preview(const ph_clip_entry *item, char *label, size_t capacity)
{
    char bytes[40];
    size_text(item->size, bytes);
    uint32_t width, height;
    if (ph_clipboard_png_size(item->data, item->size, &width, &height)) {
        snprintf(label, capacity, "Image · %u × %u · %s", (unsigned)width, (unsigned)height, bytes);
    } else {
        snprintf(label, capacity, "Image · %s", bytes);
    }
}

static void draw_clipboard_thumbnail(ph_workspace *u, const ph_clip_entry *item, SDL_Rect box)
{
    size_t slot = item->id % SDL_arraysize(u->previews);
    if (u->previews[slot].id != item->id) {
        if (u->previews[slot].texture)
            SDL_DestroyTexture(u->previews[slot].texture);
        u->previews[slot].id = item->id;
        u->previews[slot].texture = ph_clipboard_thumbnail(u->renderer, item->data, item->size);
        if (u->previews[slot].texture)
            SDL_QueryTexture(u->previews[slot].texture, NULL, NULL,
                             &u->previews[slot].width, &u->previews[slot].height);
    }
    rounded(u, box, 7, u->selected_bg);
    SDL_Texture *texture = u->previews[slot].texture;
    int width = u->previews[slot].width, height = u->previews[slot].height;
    if (texture && width > 0 && height > 0) {
        int side = SDL_min(box.w, box.h) - 6;
        int longest = SDL_max(width, height);
        width = SDL_max(1, width * side / longest);
        height = SDL_max(1, height * side / longest);
        SDL_Rect dest = {box.x + (box.w - width) / 2, box.y + (box.h - height) / 2,
                         width, height};
        SDL_RenderCopy(u->renderer, texture, NULL, &dest);
    } else {
        ph_icon_draw(u->renderer, PH_ICON_FILE,
                     (SDL_Rect){box.x + (box.w - 21) / 2, box.y + (box.h - 21) / 2, 21, 21},
                     u->muted);
    }
}

static void clipboard_row(ph_workspace *u, SDL_Rect row, const ph_clip_entry *item,
                          bool stacked, bool send)
{
    enum { COPY_W = 65, SEND_W = 106, DELETE_W = 55, GAP = 8 };
    bool checked = clip_checked(u, item->id);
    if (checked)
        rounded(u, row, 6, u->selected_bg);

    SDL_Rect check = {row.x + 3, row.y + 24, 16, 16};
    rounded(u, check, 4, checked ? u->ink : u->muted);
    if (checked)
        ph_icon_draw(u->renderer, PH_ICON_CHECK, check, u->surface);
    else
        rounded(u, (SDL_Rect){check.x + 1, check.y + 1, 14, 14}, 3, u->surface);

    SDL_Rect tile = {row.x + 28, row.y + 12, 40, 40};
    if (item->kind == 'P')
        draw_clipboard_thumbnail(u, item, tile);
    else {
        rounded(u, tile, 7, u->selected_bg);
        ph_icon_draw(u->renderer, PH_ICON_CLIPBOARD,
                     (SDL_Rect){tile.x + 10, tile.y + 10, 20, 20}, u->muted);
    }
    int right = row.x + row.w - 4;
    int actions = right - COPY_W - SEND_W - DELETE_W - 2 * GAP;
    int label_x = tile.x + tile.w + 12;
    int label_right = stacked ? right : actions - 12;
    int label_width = label_right - label_x;
    char preview[200], when[40], details[128];
    if (item->kind == 'T')
        snprintf(preview, sizeof preview, "%.190s", (const char *)item->data);
    else
        image_preview(item, preview, sizeof preview);
    timestamp(item->time, when);
    snprintf(details, sizeof details, "From %s · %s", item->incoming ? "iPhone" : pc_name(u), when);
    fitted_label(u, preview, label_x, row.y + 13, label_width, 13, true, u->ink);
    fitted_label(u, details, label_x, row.y + 35, label_width, 11, false, u->muted);
    hit(u, (SDL_Rect){row.x, row.y, label_right - row.x, 58}, UI_CLIP_CHECK, item->id,
        checked ? "Deselect clipboard item" : "Select clipboard item");

    int y = row.y + (stacked ? 57 : 18);
    if (stacked)
        actions = row.x + 3;
    action_button(u, (SDL_Rect){actions, y, COPY_W, 28}, "Copy", PH_WORKSPACE_CLIP_COPY,
                  item->id, false, true, true, PH_ICON_COPY);
    button(u, (SDL_Rect){actions + COPY_W + GAP, y, SEND_W, 28}, "Send to iPhone",
           PH_WORKSPACE_CLIP_SEND, item->id, false, send);
    button(u, (SDL_Rect){right - DELETE_W, y, DELETE_W, 28}, "Delete", UI_CLIP_REMOVE,
           item->id, false, true);
}

static void clipboard_page(ph_workspace *u)
{
    char subtitle[150];
    snprintf(subtitle, sizeof subtitle, "Up to 100 recent items stored on %s.", pc_name(u));
    bool panel = u->live.mirroring && !u->live.modal;
    SDL_Rect b = history_header(u, "Clipboard history", subtitle, panel);
    bool ready = clipboard_ready(u);
    int status_y = b.y + (panel ? 67 : 112);
    status_dot(u, b.x, status_y + 6, ready);
    fitted_label(u, clipboard_status(u, false), b.x + 13, status_y, b.w - 13, 12, false,
                 ready ? u->positive : u->muted);

    size_t total = ph_history_clip_count(u->history);
    size_t kept = 0;
    for (size_t i = 0; i < u->clip_selected_n; i++)
        for (size_t j = 0; j < total; j++)
            if (ph_history_clip(u->history, j)->id == u->clip_selected[i]) {
                u->clip_selected[kept++] = u->clip_selected[i];
                break;
            }
    u->clip_selected_n = kept;

    bool stacked = b.w < 550;
    bool split_toolbar = total && b.w < 468;
    int row_height = stacked ? 94 : 66;
    int toolbar_height = split_toolbar ? 78 : 44;
    int offset = panel ? 98 : 160;
    int available = SDL_max(0, b.h - offset - 18);
    int content = total ? (int)total * row_height : 93;
    SDL_Rect list = {b.x, b.y + offset, b.w, SDL_min(available, 20 + toolbar_height + content)};
    if (list.h <= toolbar_height + 10)
        return;
    card(u, list);
    SDL_Rect rows = {list.x + 14, list.y + 10, list.w - 28, list.h - 20};
    int y = rows.y, right = rows.x + rows.w;
    char count[48];
    if (kept)
        snprintf(count, sizeof count, "%zu of %zu selected", kept, total);
    else
        snprintf(count, sizeof count, "%zu %s", total, total == 1 ? "item" : "items");
    int select_width = 87;
    int count_width = ph_text_width(count, PH_TEXT(12, PH_FONT_MEDIUM));
    text(u, count, rows.x, y + 8, count_width + 2, 12, true, u->ink);
    button(u, (SDL_Rect){right - 92, y, 92, 30}, "Clear history", UI_CLEAR, 0, false, total > 0);
    if (total) {
        int select_x = split_toolbar ? rows.x : rows.x + count_width + 17;
        int select_y = split_toolbar ? y + 34 : y;
        button(u, (SDL_Rect){select_x, select_y, select_width, 30},
               kept == total ? "Deselect all" : "Select all", UI_CLIP_CHECK, 0, false, true);
        button(u, (SDL_Rect){right - (split_toolbar ? 115 : 213), select_y, 115, 30},
               "Delete selected", UI_CLIP_DELETE, 0, false, kept > 0);
    }
    line(u, rows.x, y + toolbar_height - 10, rows.w);
    rows.y += toolbar_height;
    rows.h -= toolbar_height;
    if (!total) {
        history_empty(u, rows, PH_ICON_CLIPBOARD, "No clipboard items",
                      "Copied text and images appear here.");
        return;
    }

    SDL_RenderSetClipRect(u->renderer, &rows);
    int row_y = rows.y - u->scroll;
    for (size_t i = 0; i < total; i++, row_y += row_height) {
        if (row_y + row_height <= rows.y)
            continue;
        if (row_y >= rows.y + rows.h)
            break;
        SDL_Rect row = {rows.x, row_y, rows.w, row_height};
        clipboard_row(u, row, ph_history_clip(u->history, i), stacked, ready && u->to_phone);
        if (i + 1 < total)
            line(u, row.x + 12, row.y + row.h - 1, row.w - 24);
    }
    SDL_RenderSetClipRect(u->renderer, NULL);
    u->scroll_limit = SDL_max(content - rows.h, 0);
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
    if (u->view == PH_WORKSPACE_FILES) {
        close_button(u, b.x + b.w - 39, y + 1, PH_WORKSPACE_CLOSE);
        width -= 32;
    } else {
        text(u, "File transfers", x, y, width, 18, true, u->ink);
        y += 34;
    }
    int cell = (width - 12) / 3;
    char history_name[32];
    button(u, (SDL_Rect){x, y, cell, 30}, "Send files", PH_WORKSPACE_SEND_FILES, 0,
           true, u->live.connected);
    button(u, (SDL_Rect){x + cell + 6, y, cell, 30}, history_button_label(u, history_name),
           UI_HISTORY, 0, false, true);
    button(u, (SDL_Rect){x + 2 * (cell + 6), y, cell, 30}, "Open folder", PH_WORKSPACE_REVEAL, 0,
           false, true);
    y += 44;
    width = b.w - 24;
    y = transfer_progress(u, x, y, width);
    if (b.y + b.h > y + 90)
        draw_file_rows(u, (SDL_Rect){x, y, width, b.y + b.h - y - 12}, false);
}

static void pairing_actions(ph_workspace *u, int x, int y, int width)
{
    int small = width < 350 ? 74 : 104;
    int guide = SDL_min(150, width - 2 * small - 12);
    button(u, (SDL_Rect){x, y, guide, 34}, "Set up controls", PH_WORKSPACE_SETUP, 0, false, true);
    button(u, (SDL_Rect){x + guide + 6, y, small, 34}, "Devices",
           UI_SECTION + PH_SETTINGS_DEVICES, 0, false, true);
    button(u, (SDL_Rect){x + guide + small + 12, y, small, 34}, "Settings",
           PH_WORKSPACE_SETTINGS_OPEN, 0, false, true);
}

static void connection_status(ph_workspace *u, SDL_Rect box, bool spacious)
{
    const char *names[] = {"Wi-Fi connection", "Screen sharing", "Mouse and keyboard",
                           "Clipboard sync"};
    const char *details[] = {"Available on your local network", "Start a broadcast from the iPhone app",
                             "Inactive until screen sharing or Bluetooth setup",
                             "Available after your iPhone connects"};
    const char *states[] = {"Ready", u->live.share_checked && !u->live.share_available
                                        ? "Unavailable" : "Waiting",
                            "Inactive", "Waiting"};
    const ph_control_icon icons[] = {PH_ICON_WIFI, PH_ICON_PHONE, PH_ICON_MOUSE, PH_ICON_CLIPBOARD};
    int x = box.x + 17, y = box.y + (spacious ? 27 : 34);
    text(u, "CONNECTION STATUS", box.x, box.y, box.w, 10, true, u->muted);
    const int heights[] = {spacious ? 75 : 62, spacious ? 75 : 62,
                           spacious ? 100 : 87, spacious ? 75 : 62};
    card(u, (SDL_Rect){box.x, y, box.w, box.y + box.h - y});
    for (int i = 0; i < 4; i++) {
        if (i)
            line(u, box.x + 1, y, box.w - 2);
        int dy = spacious ? 5 : 0;
        rounded(u, (SDL_Rect){x, y + 14 + dy, 32, 32}, 8, u->background);
        ph_icon_draw(u->renderer, icons[i], (SDL_Rect){x + 7, y + 21 + dy, 18, 18}, u->ink);
        text(u, names[i], x + 44, y + 12 + dy, box.w - 145, 13, true, u->ink);
        int sw = ph_text_width(states[i], PH_TEXT(11, PH_FONT_REGULAR));
        int right = box.x + box.w - 16;
        status_dot(u, right - sw - 11, y + 18 + dy, i == 0);
        text(u, states[i], right - sw, y + 13 + dy, sw + 2, 11, false,
             i == 0 ? u->positive : u->muted);
        text(u, details[i], x + 44, y + 34 + dy, box.w - 76, 12, false, u->muted);
        if (i == 2)
            action_button(u, (SDL_Rect){x + 39, y + 56 + dy, 108, 26}, "Set up controls",
                          PH_WORKSPACE_SETUP, 0, false, true, false, -1);
        y += heights[i];
    }
}

static int sharing_hint(ph_workspace *u, int x, int y, int width)
{
    ph_icon_draw(u->renderer, PH_ICON_INFO, (SDL_Rect){x, y, 16, 16}, u->muted);
    const char *parts[] = {"Screen sharing: ", "select ", "Share screen ",
                           "on your iPhone, then ", "Start Broadcast", "."};
    int fonts[] = {PH_TEXT(12, PH_FONT_MEDIUM), PH_TEXT(12, PH_FONT_REGULAR)};
    int total = 0;
    for (size_t i = 0; i < SDL_arraysize(parts); i++)
        total += ph_text_width(parts[i], fonts[i % 2]);
    if (total + 27 <= width) {
        int left = x + 25;
        for (size_t i = 0; i < SDL_arraysize(parts); i++) {
            int advance = ph_text_width(parts[i], fonts[i % 2]);
            ph_text_draw(u->renderer, parts[i], left, y, advance + 2, fonts[i % 2],
                         i % 2 ? u->muted : u->ink);
            left += advance;
        }
        y += ph_text_line_height(fonts[0]);
    } else
        y = text(u, "Screen sharing: select Share screen on your iPhone, then Start Broadcast.",
                 x + 25, y, width - 25, 12, false, u->muted);
    return text(u, "File transfers and clipboard sync are available without screen sharing.",
                x + 25, y, width - 25, 12, false, u->muted);
}

static void pairing_field(ph_workspace *u, SDL_Rect box, const char *label, const char *value,
                          int action, bool inline_label)
{
    bool code = action == PH_WORKSPACE_COPY_CODE;
    int font = PH_TEXT(code ? 18 : 13, PH_FONT_MONO_MEDIUM);
    char formatted[12];
    if (code && strlen(value) == 8) {
        snprintf(formatted, sizeof formatted, "%.4s %.4s", value, value + 4);
        value = formatted;
    }
    bool available = code ? u->live.code[0] != 0 : u->live.address[0] != 0;
    if (inline_label) {
        text(u, label, box.x + 12, box.y + 16, 78, 12, false, u->muted);
        int width = SDL_min(ph_text_width(value, font) + 1, box.w - 123);
        fitted_text(u, value, box.x + box.w - 44 - width,
                    box.y + (box.h - ph_text_line_height(font)) / 2, width, font, u->ink);
    } else {
        text(u, label, box.x + 12, box.y + 7, box.w - 48, 10, false, u->muted);
        fitted_text(u, value, box.x + 12, box.y + 22, box.w - 48, font, u->ink);
    }
    copy_button(u, (SDL_Rect){box.x + box.w - 36, box.y + 10, 28, 30}, action, available);
}

static void wide_pairing(ph_workspace *u, int x, int y, int width, bool spacious)
{
    int gap = 24, pair_width = (width - gap) * 57 / 100;
    SDL_Rect pair = {x, y, pair_width, spacious ? 352 : 307},
             status = {x + pair_width + gap, y, width - pair_width - gap, pair.h};
    card(u, pair);
    int q = SDL_clamp(pair_width - 302, 128, spacious ? 196 : 174);
    int qr_x = x + (spacious ? 26 : 22), qr_y = y + (spacious ? 26 : 24);
    rounded(u, (SDL_Rect){qr_x, qr_y, q, q}, 9, u->border);
    rounded(u, (SDL_Rect){qr_x + 1, qr_y + 1, q - 2, q - 2}, 8,
            (SDL_Color){255, 255, 255, 255});
    if (u->live.code[0])
        qr(u, (SDL_Rect){qr_x + 7, qr_y + 7, q - 14, q - 14});
    text(u, "Scan with the Porthole iPhone app", qr_x, qr_y + q + 12, q, 11, false, u->muted);
    int dx = qr_x + q + (spacious ? 23 : 24);
    int dw = pair.x + pair.w - (spacious ? 26 : 22) - dx;
    text(u, "COMPUTER NAME", dx, y + (spacious ? 26 : 19), dw, 11, true, u->muted);
    fitted_label(u, pc_name(u), dx, y + (spacious ? 45 : 37),
                 dw, 23, true, u->ink);
    int fields_y = y + (spacious ? 96 : 79);
    rounded(u, (SDL_Rect){dx, fields_y, dw, 100}, 8, u->border);
    rounded(u, (SDL_Rect){dx + 1, fields_y + 1, dw - 2, 98}, 7, u->surface);
    line(u, dx + 1, fields_y + 50, dw - 2);
    pairing_field(u, (SDL_Rect){dx, fields_y, dw, 50}, "Address",
                  u->live.address[0] ? u->live.address : "Starting…",
                  PH_WORKSPACE_COPY_ADDRESS, spacious);
    pairing_field(u, (SDL_Rect){dx, fields_y + 50, dw, 50}, "Code", u->live.code,
                  PH_WORKSPACE_COPY_CODE, spacious);
    int below = text(u, "Scan the QR code or enter the address and connection code in the iPhone app.",
                     dx, fields_y + 117, dw, 12, false, u->muted);
    button(u, (SDL_Rect){dx, below + 14, spacious ? 86 : 100, spacious ? 39 : 34},
           "New code", PH_WORKSPACE_NEW_CODE, 0, false,
           u->live.code[0] != 0);
    connection_status(u, status, spacious);
    int end = sharing_hint(u, x, y + pair.h + 24, width);
    int footer = SDL_max(end + 14, u->rect.y + u->rect.h - 41 - u->scroll);
    if (spacious)
        line(u, u->rect.x, footer - 4, u->rect.w);
    action_button(u, (SDL_Rect){x, footer, 158, 34}, "Certificate fingerprint", UI_FINGERPRINT,
                  0, false, true, false, -1);
    action_button(u, (SDL_Rect){x + width - 272, footer, 148, 34}, "Connection options",
                  UI_SECTION + PH_SETTINGS_DEVICES, 0, false, true, false, PH_ICON_SLIDERS);
    action_button(u, (SDL_Rect){x + width - 110, footer, 110, 34}, "Settings",
                  PH_WORKSPACE_SETTINGS_OPEN, 0, false, true, !spacious,
                  spacious ? -1 : PH_ICON_GEAR);
    u->scroll_limit = SDL_max(0, end + 56 + u->scroll - u->rect.y - u->rect.h);
}

static void paired_features(ph_workspace *u, SDL_Rect box)
{
    const char *names[] = {"Screen sharing", "Mouse and keyboard", "Clipboard sync"};
    const char *compact_names[] = {"Screen", "Controls", "Clipboard"};
    const char *states[] = {u->live.mirroring ? "Active"
                              : !u->live.connected ? "Disconnected"
                              : u->live.share_checked && !u->live.share_available ? "Unavailable"
                                                                                 : "Off",
                            u->live.controls && !u->live.paused ? "Active" : "Inactive",
                            clipboard_status(u, true)};
    const bool active[] = {u->live.mirroring, u->live.controls && !u->live.paused,
                           clipboard_ready(u)};
    const ph_control_icon icons[] = {PH_ICON_PHONE, PH_ICON_MOUSE, PH_ICON_CLIPBOARD};
    card(u, box);
    for (int i = 0; i < 3; i++) {
        int x = box.x + i * box.w / 3, w = box.x + (i + 1) * box.w / 3 - x;
        int right = x + w - (box.w < 450 ? 9 : 18);
        const char *label = box.w < 450 ? compact_names[i] : names[i];
        int label_width = ph_text_width(label, PH_TEXT(12, PH_FONT_MEDIUM));
        int state_width = ph_text_width(states[i], PH_TEXT(11, PH_FONT_REGULAR));
        bool inline_state = label_width + state_width + 87 < w;
        int left = x + (box.w < 450 ? 10 : 16), title_y = box.y + (inline_state ? 16 : 8);
        if (i)
            rect(u, (SDL_Rect){x, box.y + 1, 1, box.h - 2}, u->border);
        ph_icon_draw(u->renderer, icons[i], (SDL_Rect){left, title_y - 1, 17, 17}, u->muted);
        fitted_label(u, label, left + 23, title_y, right - left - 23, 12, true, u->ink);
        int state_x = inline_state ? right - 19 - state_width : left + 23;
        int state_y = box.y + (inline_state ? 17 : 30);
        status_dot(u, state_x - 10, state_y + 5, active[i]);
        fitted_label(u, states[i], state_x, state_y, right - state_x, 11, false,
                     active[i] ? u->positive : u->muted);
        if (box.w >= 450)
            ph_icon_draw(u->renderer, PH_ICON_CHEVRON,
                         (SDL_Rect){right - 8, box.y + 19, 11, 11}, u->muted);
        hit(u, (SDL_Rect){x, box.y, w, box.h},
            i == 2 ? PH_WORKSPACE_CLIPBOARD_OPEN : UI_SECTION + (i == 0 ? 1 : 2), 0, names[i]);
    }
}

static void drop_zone(ph_workspace *u, SDL_Rect box)
{
    SDL_Color edge = ph_workspace_dark(u) ? (SDL_Color){87, 101, 120, 255}
                                          : (SDL_Color){181, 193, 210, 255};
    rounded(u, box, 10, edge);
    rounded(u, (SDL_Rect){box.x + 1, box.y + 1, box.w - 2, box.h - 2}, 9, u->background);
    /* Paint over short intervals on the straight sides, keeping the rounded corners smooth. */
    float sx, sy;
    SDL_RenderGetScale(u->renderer, &sx, &sy);
    color(u->renderer, u->background);
    for (int vertical = 0; vertical < 2; vertical++) {
        int length = vertical ? box.h : box.w;
        SDL_FRect gaps[48];
        int n = 0;
        for (int p = 14; p + 4 <= length - 11; p += 9) {
            for (int side = 0; side < 2; side++) {
                int x = box.x + (vertical ? side * (box.w - 1) : p);
                int y = box.y + (vertical ? p : side * (box.h - 1));
                int px = (int)lroundf(x * sx), py = (int)lroundf(y * sy);
                int pw = (int)lroundf((x + (vertical ? 1 : 4)) * sx) - px;
                int ph = (int)lroundf((y + (vertical ? 4 : 1)) * sy) - py;
                gaps[n++] = ph_ui_pixel_rect(px, py, pw, ph, sx, sy);
            }
            if (n == (int)SDL_arraysize(gaps)) {
                SDL_RenderFillRectsF(u->renderer, gaps, n);
                n = 0;
            }
        }
        if (n)
            SDL_RenderFillRectsF(u->renderer, gaps, n);
    }
}

static const char *pairing_recovery(const ph_workspace *u)
{
    switch (u->live.pairing_status) {
    case PH_ORIGIN_START_IDENTITY:
        return "Porthole couldn’t open its connection credentials. Check that your app data "
               "folder is accessible, then try again.";
    case PH_ORIGIN_START_LISTENER:
        return "The pairing port is in use or blocked. Quit any other copy of Porthole, "
               "including in the system tray, then try again.";
    case PH_ORIGIN_START_FAILED:
        return "Porthole couldn’t start pairing. Try again.";
    default:
        return "Connect this PC to the same local network as your iPhone using Wi-Fi or Ethernet, "
               "then try again.";
    }
}

static void home(ph_workspace *u)
{
    SDL_Rect b = u->rect;
    int pad = b.w < 600 ? 20 : 44, x = b.x + pad, width = b.w - 2 * pad, y = b.y + 30;
    if (!u->live.paired && !u->live.address[0] && u->live.share_checked) {
        y = text(u, "Connect your iPhone", x, y, width, 30, true, u->ink) + 18;
        int paragraph = SDL_min(width - 32, 525);
        const char *recovery = pairing_recovery(u);
        int height = 115 + ph_text_height(recovery, paragraph, PH_TEXT(13, PH_FONT_REGULAR));
        card(u, (SDL_Rect){x, y, SDL_min(width, 565), height});
        y = text(u, "Pairing unavailable", x + 16, y + 15, paragraph, 17, true, u->ink) + 7;
        y = text(u, recovery, x + 16, y, paragraph, 13, false, u->muted) + 20;
        button(u, (SDL_Rect){x + 16, y, 92, 34}, "Try again", PH_WORKSPACE_RETRY_PAIRING,
               0, true, true);
        button(u, (SDL_Rect){x + 116, y, 86, 34}, "Devices", UI_SECTION + PH_SETTINGS_DEVICES,
               0, false, true);
        return;
    }
    if (!u->live.paired) {
        bool spacious = b.w >= 1150 && b.h >= 660 && u->live.address[0];
        if (spacious) {
            pad = 62;
            x = b.x + pad;
            width = b.w - 2 * pad;
            y = b.y + 46;
        }
        y -= u->scroll;
        bool wide = width >= 810 && b.h >= 545 && u->live.address[0];
        if (spacious) {
            ph_text_draw(u->renderer, "Connect your iPhone", x, y, width,
                         PH_TEXT(38, PH_FONT_SEMIBOLD), u->ink);
            y += 55;
        } else
            y = text(u, "Connect your iPhone", x, y, width, wide ? 37 : width < 350 ? 24 : 30,
                     true, u->ink) + 10;
        const char *instructions = "Open Porthole on your iPhone to scan the QR code or enter the "
                                   "pairing details. Keep both devices on the same local network.";
        y = text(u, instructions, x, y, width, spacious ? 15 : 14, false, u->muted)
            + (spacious ? 34 : 26);
        if (wide) {
            wide_pairing(u, x, y, width, spacious);
            return;
        }
        if (width < 350) {
            card(u, (SDL_Rect){x, y, width, 194});
            int q = 100, dx = x + q + 20;
            if (u->live.code[0])
                qr(u, (SDL_Rect){x + 6, y + 8, q, q});
            text(u, "COMPUTER NAME", dx, y + 18, width - q - 32, 10, true, u->muted);
            fitted_label(u, pc_name(u), dx, y + 36, width - q - 32, 15,
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
            int end = sharing_hint(u, x, y + 51, width);
            u->scroll_limit = SDL_max(0, end + 16 + u->scroll - b.y - b.h);
            return;
        }
        SDL_Rect pair = {x, y, width, 250};
        card(u, pair);
        int q = b.w < 620 ? 116 : 168, dx = x + q + 40;
        if (u->live.code[0])
            qr(u, (SDL_Rect){x + 18, y + 26, q, q});
        text(u, "COMPUTER NAME", dx, y + 24, width - q - 64, 11, true, u->muted);
        text(u, pc_name(u), dx, y + 44, width - q - 64, 22, true,
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
        int end = sharing_hint(u, x, y + 51, width);
        u->scroll_limit = SDL_max(0, end + 16 + u->scroll - b.y - b.h);
        return;
    }
    card(u, (SDL_Rect){x, y, 48, 58});
    ph_icon_draw(u->renderer, PH_ICON_PHONE, (SDL_Rect){x + 8, y + 13, 32, 32}, u->ink);
    fitted_label(u, u->live.phone[0] ? u->live.phone : "iPhone", x + 66, y,
                 width - (width < 550 ? 76 : 348), 28, true, u->ink);
    status_dot(u, x + 66, y + 44, u->live.connected);
    text(u, u->live.connected ? "Connected" : "Disconnected", x + 80, y + 38, 100, 12, false,
         u->live.connected ? u->positive : u->muted);
    if (u->live.connected)
        text(u, "Wi-Fi", x + 154, y + 38, 55, 12, false, u->muted);
    int button_y = y + (width < 550 ? 65 : 10);
    action_button(u, (SDL_Rect){x + width - 272, button_y, 148, 34}, "Connection options",
                  UI_SECTION + PH_SETTINGS_DEVICES, 0, false, true, false, PH_ICON_SLIDERS);
    action_button(u, (SDL_Rect){x + width - 110, button_y, 110, 34}, "Settings",
                  PH_WORKSPACE_SETTINGS_OPEN, 0, false, true, true, PH_ICON_GEAR);
    y += width < 550 ? 115 : 78;
    paired_features(u, (SDL_Rect){x, y, width, 52});
    y += 74;
    if (!u->live.connected) {
        y = text(u, "To send files from your iPhone, choose Porthole in Photos or Files under Share.",
                 x, y, width, 14, false, u->muted) + 18;
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
    int needed = 58 + 113 + 44 + SDL_max(rows, 1) * 66 + 55;
    if (u->live.active_file[0])
        needed += 38;
    if (u->live.incoming_file[0])
        needed += 60;
    else if (u->live.receiving_total)
        needed += 30;
    SDL_Rect files = {x, y, width, SDL_min(needed, available)};
    card(u, files);
    text(u, "File transfers", x + 20, y + 18, width - 290, 18, true, u->ink);
    char history_name[32];
    const char *folder = u->live.folder[0] ? u->live.folder : "Downloads";
    char open_folder[160] = "Open folder";
    if (strcmp(folder, "Folder unavailable"))
        snprintf(open_folder, sizeof open_folder, "Open %s", folder);
    int folder_width = SDL_clamp(ph_text_width(open_folder, PH_TEXT(12, PH_FONT_MEDIUM)) + 32,
                                 115, 175);
    action_button(u, (SDL_Rect){x + width - folder_width - 123, y + 12, 96, 32},
                  history_button_label(u, history_name), UI_HISTORY, 0, false, true, false, -1);
    action_button(u, (SDL_Rect){x + width - folder_width - 15, y + 12, folder_width, 32},
                  open_folder, PH_WORKSPACE_REVEAL, 0, false, true, false, PH_ICON_FOLDER);
    y += 58;
    int drop_height = b.y + b.h - y >= 285 ? 94 : 74;
    drop_zone(u, (SDL_Rect){x + 19, y, width - 38, drop_height});
    ph_icon_draw(u->renderer, PH_ICON_SEND,
                 (SDL_Rect){x + 39, y + (drop_height - 34) / 2, 34, 34}, u->ink);
    text(u, "Send to iPhone", x + 90, y + drop_height / 2 - 22, width - 244,
         15, true, u->ink);
    text(u, "Drop files here or browse for files.", x + 90, y + drop_height / 2 + 2,
         width - 244, 11, false, u->muted);
    button(u, (SDL_Rect){x + width - 133, y + (drop_height - 36) / 2, 96, 36}, "Send files",
           PH_WORKSPACE_SEND_FILES, 0, true, true);
    y += drop_height + 19;
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
    action_button(u, (SDL_Rect){x + width - 126, files.y + files.h - 34, 106, 28},
                  "Change folder", PH_WORKSPACE_FOLDER, 0, false, true, false, -1);
}
typedef enum { SETTING_VALUE, SETTING_TOGGLE, SETTING_BUTTON, SETTING_SEGMENTS, SETTING_VOLUME } setting_kind;
typedef struct {
    const char *title, *detail, *value, *alternate;
    setting_kind kind;
    int action;
    bool on, enabled;
} setting_row;

static setting_row value_row(const char *title, const char *detail, const char *value)
{
    return (setting_row){.title = title, .detail = detail, .value = value, .kind = SETTING_VALUE};
}

static setting_row toggle_row(const char *title, const char *detail, int action, bool on)
{
    return (setting_row){.title = title, .detail = detail, .kind = SETTING_TOGGLE,
                         .action = action, .on = on, .enabled = true};
}

static setting_row button_row(const char *title, const char *detail, const char *label, int action,
                              bool enabled)
{
    return (setting_row){.title = title, .detail = detail, .value = label, .kind = SETTING_BUTTON,
                         .action = action, .enabled = enabled};
}

static setting_row segment_row(const char *title, const char *detail, const char *first,
                               const char *second, int action, bool second_selected)
{
    return (setting_row){.title = title, .detail = detail, .value = first, .alternate = second,
                         .kind = SETTING_SEGMENTS, .action = action, .on = second_selected};
}

static int segments_width(const setting_row *row)
{
    int font = PH_TEXT(11, PH_FONT_REGULAR);
    return ph_text_width(row->value, font) + ph_text_width(row->alternate, font) + 54;
}

static void segments(ph_workspace *u, const setting_row *row, int x, int y)
{
    rounded(u, (SDL_Rect){x, y, segments_width(row), 35}, 7, u->background);
    x += 3;
    const char *names[] = {row->value, row->alternate};
    for (int i = 0; i < 2; i++) {
        bool selected = i == (int)row->on;
        int font = PH_TEXT(11, selected ? PH_FONT_MEDIUM : PH_FONT_REGULAR);
        int width = 24 + ph_text_width(names[i], PH_TEXT(11, PH_FONT_REGULAR));
        SDL_Rect b = {x, y + 3, width, 29};
        if (selected)
            rounded(u, b, 5, u->surface);
        ph_text_draw(u->renderer, names[i], x + 12, y + 9, width - 12, font,
                     selected ? u->ink : u->muted);
        if (!selected)
            hit(u, b, row->action, (uint64_t)i, names[i]);
        x += width;
    }
}

static int row_control_width(const setting_row *row)
{
    if (row->kind == SETTING_TOGGLE)
        return 36;
    if (row->kind == SETTING_SEGMENTS)
        return segments_width(row);
    if (row->kind == SETTING_VOLUME)
        return 116;
    return SDL_min(210, ph_text_width(row->value, PH_TEXT(12, PH_FONT_MEDIUM)) +
                         (row->kind == SETTING_BUTTON ? 30 : 0));
}

static int setting_row_body(ph_workspace *u, SDL_Rect box, const setting_row *row, bool draw)
{
    int controls = row_control_width(row);
    bool stacked = box.w - controls < 224 && row->kind != SETTING_TOGGLE;
    int width = box.w - 36 - (stacked ? 0 : controls + 18);
    int heading = ph_text_height(row->title, width, PH_TEXT(13, PH_FONT_MEDIUM));
    int detail = row->detail ? 3 + ph_text_height(row->detail, width, PH_TEXT(12, PH_FONT_REGULAR))
                             : 0;
    int height = SDL_max(67, 24 + heading + detail + (stacked ? 45 : 0));
    if (!draw)
        return height;
    int label_y = box.y + (height - heading - detail - (stacked ? 45 : 0)) / 2;
    int center = stacked ? box.y + height - 29 : box.y + height / 2;
    int control_x = box.x + box.w - 18 - controls;
    text(u, row->title, box.x + 18, label_y, width, 13, true, u->ink);
    if (row->detail)
        text(u, row->detail, box.x + 18, label_y + heading + 3, width, 12, false, u->muted);
    switch (row->kind) {
    case SETTING_VALUE:
        fitted_label(u, row->value, control_x, center - 9, controls + 2, 12, false, u->muted);
        break;
    case SETTING_TOGGLE:
        toggle(u, control_x, center - 10, row->title, row->on, row->action, row->enabled);
        break;
    case SETTING_BUTTON:
        button(u, (SDL_Rect){control_x, center - 17, controls, 34}, row->value, row->action, 0,
               false, row->enabled);
        break;
    case SETTING_SEGMENTS:
        segments(u, row, control_x, center - 17);
        break;
    case SETTING_VOLUME:
        button(u, (SDL_Rect){control_x, center - 15, 30, 30}, "−", PH_WORKSPACE_VOLUME_DOWN, 0,
               false, u->live.volume > 0);
        fitted_label(u, row->value, control_x + 39, center - 9, 43, 12, true, u->muted);
        button(u, (SDL_Rect){control_x + 86, center - 15, 30, 30}, "+", PH_WORKSPACE_VOLUME_UP, 0,
               false, u->live.volume < 100);
        break;
    }
    return height;
}

static int setting_group(ph_workspace *u, int x, int y, int width, const char *title,
                         const setting_row *rows, size_t count)
{
    text(u, title, x, y, width, 10, true, u->muted);
    y += 23;
    SDL_Rect group = {x, y, width, 2};
    for (size_t i = 0; i < count; i++)
        group.h += setting_row_body(u, group, &rows[i], false);
    card(u, group);
    y++;
    for (size_t i = 0; i < count; i++) {
        if (i)
            line(u, x + 1, y, width - 2);
        y += setting_row_body(u, (SDL_Rect){x, y, width, 0}, &rows[i], true);
    }
    return y + 24;
}

static int settings_heading(ph_workspace *u, int x, int y, int width, bool inline_settings,
                            const char *title, const char *subtitle)
{
    ph_text_draw(u->renderer, title, x, y, width,
                 PH_TEXT(inline_settings ? 23 : 27, PH_FONT_SEMIBOLD), u->ink);
    y += inline_settings ? 34 : 41;
    if (subtitle)
        y = text(u, subtitle, x, y, width, inline_settings ? 11 : 12, false, u->muted);
    return y + 24;
}

/* The System thumbnail uses the same diagonal split as the preview. */
static void theme_split(ph_workspace *u, SDL_Rect b, float top, float bottom, SDL_Color dark)
{
    SDL_Vertex polygon[] = {
        {.position = {b.x + top * b.w, b.y}, .color = dark},
        {.position = {b.x + b.w, b.y}, .color = dark},
        {.position = {b.x + b.w, b.y + b.h}, .color = dark},
        {.position = {b.x + bottom * b.w, b.y + b.h}, .color = dark}
    };
    const int triangles[] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(u->renderer, NULL, polygon, 4, triangles, 6);
}

static void theme_thumbnail(ph_workspace *u, SDL_Rect b, int theme)
{
    SDL_Color light_panel = {255, 255, 255, 255}, dark_panel = {36, 42, 50, 255};
    SDL_Color light_edge = {217, 222, 230, 255}, dark_edge = {65, 73, 86, 255};
    SDL_Color edge = theme == 1 ? dark_edge : light_edge;
    SDL_Color panel = theme == 1 ? dark_panel : light_panel;
    SDL_Color dark_canvas = {21, 24, 28, 255};
    rounded(u, b, 5, edge);
    rounded(u, (SDL_Rect){b.x + 1, b.y + 1, b.w - 2, b.h - 2}, 4,
            theme == 1 ? dark_canvas : (SDL_Color){240, 242, 245, 255});
    if (theme == 2)
        theme_split(u, (SDL_Rect){b.x + 1, b.y + 4, b.w - 3, b.h - 8}, .62f, .4f, dark_canvas);
    rounded(u, (SDL_Rect){b.x + 1, b.y + 1, b.w - 2, 13}, 4, panel);
    if (theme == 2)
        theme_split(u, (SDL_Rect){b.x + 1, b.y + 2, b.w - 3, 11}, .62f, .57f, dark_panel);
    rounded(u, (SDL_Rect){b.x + 8, b.y + 5, 4, 4}, 2, (SDL_Color){50, 145, 93, 255});
    rect(u, (SDL_Rect){b.x + 1, b.y + 13, b.w - 2, 1}, edge);
    rect(u, (SDL_Rect){b.x + 1, b.y + 14, b.w / 4, b.h - 17}, panel);
    rect(u, (SDL_Rect){b.x + b.w / 4, b.y + 14, 1, b.h - 16}, edge);
    SDL_Rect example = {b.x + b.w / 3, b.y + 25, b.w * 58 / 100,
                        SDL_max(15, b.h - 38)};
    rounded(u, example, 3, theme == 2 ? (SDL_Color){119, 130, 146, 255} : edge);
    rounded(u, (SDL_Rect){example.x + 1, example.y + 1, example.w - 2, example.h - 2}, 2, panel);
    if (theme == 2)
        theme_split(u, (SDL_Rect){example.x + 1, example.y + 1, example.w - 2, example.h - 2},
                    .43f, .26f, dark_panel);
}

static int appearance_settings(ph_workspace *u, int x, int y, int width, bool inline_settings)
{
    text(u, "APPEARANCE", x, y, width, 10, true, u->muted);
    y += 23;
    int thumb_h = inline_settings || width < 450 ? 54 : 73;
    int gap = width < 450 ? 9 : 13, padding = width < 450 ? 13 : 18;
    int column = (width - 2 * padding - 2 * gap) / 3;
    int tile_h = thumb_h + 48;
    int copy_width = width - 2 * padding;
    const char *subtitle = "Choose an appearance for Porthole.";
    const char *note = "System follows your light or dark appearance setting.";
    int subtitle_h = ph_text_height(subtitle, copy_width, PH_TEXT(12, PH_FONT_REGULAR));
    int note_h = ph_text_height(note, copy_width, PH_TEXT(11, PH_FONT_REGULAR));
    int tile_y = y + 58 + subtitle_h;
    int height = tile_y - y + tile_h + 13 + note_h + 20;
    card(u, (SDL_Rect){x, y, width, height});
    text(u, "Theme", x + padding, y + 18, copy_width, 13, true, u->ink);
    text(u, subtitle, x + padding, y + 40, copy_width, 12, false, u->muted);
    const char *themes[] = {"Light", "Dark", "System"};
    for (int i = 0; i < 3; i++) {
        SDL_Rect tile = {x + padding + i * (column + gap), tile_y, column, tile_h};
        bool selected = u->theme == i;
        int edge = selected ? 2 : 1;
        rounded(u, tile, 9, selected ? u->ink : u->border);
        rounded(u, (SDL_Rect){tile.x + edge, tile.y + edge, tile.w - 2 * edge, tile.h - 2 * edge},
                9 - edge, u->surface);
        theme_thumbnail(u, (SDL_Rect){tile.x + 10, tile.y + 10, tile.w - 20, thumb_h}, i);
        text(u, themes[i], tile.x + 12, tile.y + thumb_h + 21, tile.w - 32, 12, true, u->ink);
        SDL_Rect radio = {tile.x + tile.w - 24, tile.y + thumb_h + 22, 13, 13};
        rounded(u, radio, 7, selected ? u->ink : u->border);
        int dot = selected ? 4 : 1;
        rounded(u, (SDL_Rect){radio.x + dot, radio.y + dot, radio.w - 2 * dot, radio.h - 2 * dot},
                7 - dot, u->surface);
        hit(u, tile, UI_THEME + i, 0, themes[i]);
    }
    text(u, note, x + padding, tile_y + tile_h + 13, copy_width, 11, false, u->muted);
    return y + height + 23;
}

static bool guide_matches(const help_guide *guide, const char *query)
{
    if (contains(guide->title, query) || contains(guide->summary, query) || contains(guide->note, query))
        return true;
    for (size_t i = 0; i < SDL_arraysize(guide->steps); i++)
        if (contains(guide->steps[i], query))
            return true;
    return false;
}

static int guide_contents(ph_workspace *u, const help_guide *guide, size_t index, SDL_Rect box,
                          bool draw)
{
    int left = box.x + (box.w < 400 ? 40 : 61), right = box.x + box.w - 18;
    int y = box.y + 69;
    for (size_t i = 0; i < SDL_arraysize(guide->steps); i++) {
        char copy[240];
        const char *step = guide->steps[i];
        if (index == 1 && i == 0) {
            snprintf(copy, sizeof copy, "Connect your iPhone to %s in Porthole.", pc_name(u));
            step = copy;
        }
        if (draw) {
            char number[8];
            snprintf(number, sizeof number, "%u.", (unsigned)i + 1);
            text(u, number, left, y, 22, 12, false, u->muted);
            text(u, step, left + 19, y, right - left - 19, 12, false, u->muted);
        }
        y += ph_text_height(step, right - left - 19, PH_TEXT(12, PH_FONT_REGULAR)) + 6;
    }
    y += 8;
    if (draw)
        text(u, guide->note, left, y, right - left, 11, false, u->muted);
    y += ph_text_height(guide->note, right - left, PH_TEXT(11, PH_FONT_REGULAR)) + 14;
    if (index == 3) {
        if (draw)
            button(u, (SDL_Rect){left, y, 156, 31}, "Open control settings", UI_SECTION + 2, 0,
                   false, true);
        y += 44;
    }
    return y - box.y;
}

static int draw_guides(ph_workspace *u, int x, int y, int width)
{
    bool any = false;
    for (size_t i = 0; i < SDL_arraysize(help_guides); i++) {
        const help_guide *g = &help_guides[i];
        if (!guide_matches(g, u->search))
            continue;
        any = true;
        bool open = u->search[0] || (u->expanded_guides & (1u << i));
        SDL_Rect box = {x, y, width, 72};
        if (open)
            box.h = guide_contents(u, g, i, box, false);
        card(u, box);
        rounded(u, (SDL_Rect){x + 16, y + 20, 32, 32}, 7, u->background);
        ph_icon_draw(u->renderer, g->icon, (SDL_Rect){x + 23, y + 27, 18, 18}, u->muted);
        fitted_label(u, g->title, x + 61, y + 17, width - 98, 13, true, u->ink);
        fitted_label(u, g->summary, x + 61, y + 40, width - 98, 11, false, u->muted);
        text(u, open ? "−" : "+", x + width - 26, y + 23, 17, 17, false, u->muted);
        if (!u->search[0])
            hit(u, (SDL_Rect){x, y, width, 65}, UI_HELP_GUIDE, i, g->title);
        if (open)
            guide_contents(u, g, i, box, true);
        y += box.h + 10;
    }
    if (!any)
        y = text(u, "No matching guides. Try a different search.", x, y + 16, width, 12, false,
                 u->muted) + 16;
    return y;
}

static bool shortcut_matches(const char *title, const help_shortcut *shortcut, const char *query)
{
    return contains(title, query) || contains(shortcut->action, query) ||
           contains(shortcut->keys, query);
}

static int draw_shortcuts(ph_workspace *u, int x, int y, int width)
{
    y = text(u, "Use the left Alt key. iPhone controls require an active Bluetooth input connection. "
                "Ctrl acts as Command when enabled in Controls. Window and capture shortcuts apply "
                "while viewing a shared screen.", x, y, width, 11, false, u->muted) + 16;
    bool any = false;
    for (size_t i = 0; i < SDL_arraysize(help_shortcut_groups); i++) {
        const char *title = help_shortcut_groups[i].title;
        const help_shortcut *rows = help_shortcut_groups[i].shortcuts;
        size_t count = help_shortcut_groups[i].count;
        int box_height = 2;
        for (size_t j = 0; j < count; j++) {
            if (!shortcut_matches(title, &rows[j], u->search))
                continue;
            int keys = ph_text_width(rows[j].keys, PH_TEXT(10, PH_FONT_MONO));
            int label = width - keys - 49;
            box_height += SDL_max(42, ph_text_height(rows[j].action, label,
                                                     PH_TEXT(12, PH_FONT_REGULAR)) + 20);
        }
        if (box_height == 2)
            continue;
        any = true;
        y = text(u, title, x, y, width, 11, true, u->muted) + 8;
        card(u, (SDL_Rect){x, y, width, box_height});
        int row_y = ++y;
        for (size_t j = 0; j < count; j++) {
            const help_shortcut *shortcut = &rows[j];
            if (!shortcut_matches(title, shortcut, u->search))
                continue;
            if (y > row_y)
                line(u, x + 1, y, width - 2);
            int keys = ph_text_width(shortcut->keys, PH_TEXT(10, PH_FONT_MONO));
            int label = width - keys - 49;
            int h = SDL_max(42, ph_text_height(shortcut->action, label,
                                               PH_TEXT(12, PH_FONT_REGULAR)) + 20);
            text(u, shortcut->action, x + 14, y + 11, label, 12, false, u->ink);
            ph_text_draw(u->renderer, shortcut->keys, x + width - keys - 16, y + (h - 15) / 2,
                         keys + 2, PH_TEXT(10, PH_FONT_MONO), u->muted);
            y += h;
        }
        y += 23;
    }
    if (!any)
        y = text(u, "No matching shortcuts. Try a different search.", x, y + 16, width, 12,
                 false, u->muted) + 16;
    return y;
}

static int help_settings(ph_workspace *u, int x, int y, int width)
{
    entry_box(u, (SDL_Rect){x, y, width, 39}, u->search, "Search help and shortcuts", UI_SEARCH);
    y += 54;
    const char *tabs[] = {"Guides", "Keyboard shortcuts"};
    int tab_x = x;
    line(u, x, y + 41, width);
    for (int i = 0; i < 2; i++) {
        bool active = (int)u->shortcuts == i;
        int font = PH_TEXT(12, active ? PH_FONT_MEDIUM : PH_FONT_REGULAR);
        int w = ph_text_width(tabs[i], font);
        ph_text_draw(u->renderer, tabs[i], tab_x, y + 12, w + 2, font,
                     active ? u->ink : u->muted);
        if (active)
            rect(u, (SDL_Rect){tab_x, y + 40, w, 2}, u->ink);
        hit(u, (SDL_Rect){tab_x, y, w + 5, 42}, i ? UI_SHORTCUTS : UI_GUIDES, 0, tabs[i]);
        tab_x += w + 23;
    }
    y += 62;
    return u->shortcuts ? draw_shortcuts(u, x, y, width) : draw_guides(u, x, y, width);
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
    bool sharing = d && d->screen_sharing;
    int h = width < 420 ? 148 : 126;
    card(u, (SDL_Rect){x, y, width, h});
    text(u, *name ? name : "iPhone", x + 14, y + 10, width - 28, 15, true, u->ink);
    text(u,
         sharing && !connected ? "iPhone app inactive"
         : companion ? "Porthole companion · Files and clipboard"
         : d->usb  ? "USB · Screen only"
                   : "AirPlay · Screen only",
         x + 14, y + 35, width - 28, 12, false, u->muted);
    const char *status = connected ? "Connected" : sharing ? "Screen sharing"
                       : companion && d && !d->active ? "Saved" : "Disconnected";
    text(u, status, x + 14, y + 56, width - 28, 11, false,
         connected || sharing ? u->positive : u->muted);
    int dx = x + 12, dy = y + 83;
    if (d && (companion || d->usb || connected)) {
        const char *action = d->active ? "Selected" : companion ? "Select iPhone"
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
        if (connected || sharing) {
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
static int general_settings(ph_workspace *u, int x, int y, int width, bool inline_settings)
{
    y = appearance_settings(u, x, y, width, inline_settings);
    setting_row rows[] = {
        toggle_row("Keep Porthole available in the system tray",
#ifdef _WIN32
                   "Access connection and control options from the notification area.",
#else
                   "Keep transfers available when you close the window.",
#endif
                   UI_TRAY, u->tray),
        toggle_row("Launch at sign-in", u->launch_managed
                       ? "Managed by the Windows installer for all users."
                       : "Start Porthole automatically when you sign in.",
                   UI_SIGN_IN, u->launch_at_sign_in || u->launch_managed)
    };
    rows[1].enabled = !u->launch_managed;
    return setting_group(u, x, y, width, "APPLICATION", rows, SDL_arraysize(rows));
}

static int screen_settings(ph_workspace *u, int x, int y, int width)
{
    setting_row screen[] = {
        toggle_row("Keep the iPhone aspect ratio",
                   "Preserve the full phone display when resizing the window.",
                   UI_KEEP_ASPECT, u->keep_aspect),
        toggle_row("Always on top", "Keep the sharing window above other applications.",
                   PH_WORKSPACE_ALWAYS_ON_TOP, u->live.always_on_top)
    };
    y = setting_group(u, x, y, width, "SCREEN", screen, SDL_arraysize(screen));
    char volume[16];
    snprintf(volume, sizeof volume, "%d%%", u->live.volume);
    setting_row audio[] = {
        toggle_row("PC audio", "Play audio from screen sharing and Screen Mirroring on this PC.",
                   PH_WORKSPACE_SOUND, u->live.sound),
        {.title = "Output volume", .value = volume, .kind = SETTING_VOLUME}
    };
    y = setting_group(u, x, y, width, "AUDIO", audio, SDL_arraysize(audio));
    setting_row capture[] = {
        button_row("Full screen", NULL, "Full screen", PH_WORKSPACE_FULLSCREEN, u->live.mirroring),
        button_row("Screenshot", NULL, "Save screenshot", PH_WORKSPACE_CAPTURE, u->live.mirroring),
        button_row("Recording", NULL, u->live.recording ? "Stop recording" : "Start recording",
                   PH_WORKSPACE_RECORD, u->live.mirroring || u->live.recording),
        button_row("Saved captures", NULL, "Open Captures folder", PH_WORKSPACE_CAPTURES, true)
    };
    return setting_group(u, x, y, width, "CAPTURE AND DISPLAY", capture, SDL_arraysize(capture));
}

static bool verification_required(const ph_workspace *u)
{
    return !u->live.paused && u->live.connected && u->live.phone_controls_managed &&
           u->live.phone_controls_allowed && !u->live.phone_controls_verified;
}

static const char *controls_status(const ph_workspace *u)
{
    if (u->live.paused)
        return "Disabled on this PC";
    if (u->live.phone_controls_managed && !u->live.phone_controls_allowed)
        return "Disabled on your iPhone";
    if (u->live.controls)
        return u->live.mirroring ? "Ready · Move the mouse over your iPhone preview"
                                 : "Mouse and keyboard enabled";
    if (verification_required(u))
        return "Verification required · Tap Verify Bluetooth on your iPhone";
    if (u->live.phone_controls_verified)
        return u->live.mirroring ? "Bluetooth verified · Waiting for input"
                                 : "Bluetooth verified · Waiting for screen sharing";
    if (u->live.mirroring)
        return "Enabled · Waiting for Bluetooth input";
    return "Enabled · Waiting for screen sharing";
}

static int control_settings(ph_workspace *u, int x, int y, int width)
{
    const char *activation = !u->live.phone_controls_managed || u->live.phone_controls_screen_only
                                 ? "Screen sharing only" : "When connected";
    setting_row connection[] = {
        toggle_row("Enable PC controls", "Allow this PC’s mouse and keyboard to control your iPhone.",
                   PH_WORKSPACE_CONTROLS, !u->live.paused),
        value_row("Activation", "Managed under Mouse and keyboard in the iPhone app.", activation),
        button_row("Bluetooth and pointer setup", "Pair over Bluetooth and check pointer alignment.",
                   "Set up controls", PH_WORKSPACE_SETUP, true),
        value_row("Bluetooth device", NULL, u->live.bluetooth_peer[0] ? u->live.bluetooth_peer
                                                                       : "Name unavailable")
    };
    y = setting_group(u, x, y, width, "CONNECTION", connection,
                      SDL_arraysize(connection) - !u->live.phone_controls_verified);
    if (u->live.mirroring) {
        setting_row sharing = toggle_row("Show control toolbar",
                                         "Display navigation and volume buttons beside your iPhone.",
                                         PH_WORKSPACE_TOOLBAR, u->live.toolbar);
        y = setting_group(u, x, y, width, "SHARING WINDOW", &sharing, 1);
    }
    setting_row pointer[] = {
        segment_row("Pointer mode", "Changes apply after restarting Porthole and pairing again.",
                    "Absolute", "Relative", PH_WORKSPACE_POINTER, u->live.relative),
        segment_row("Wheel scrolling", "Feed mode converts each scroll burst into a swipe. F4 toggles this mode.",
                    "Normal", "Feed", PH_WORKSPACE_FEED, u->live.feed)
    };
    y = setting_group(u, x, y, width, "POINTER", pointer, SDL_arraysize(pointer));
    setting_row keyboard[] = {
        toggle_row("Use Ctrl as Command", "Use familiar shortcuts such as Ctrl+C, Ctrl+V and Ctrl+A.",
                   PH_WORKSPACE_CTRL_COMMAND, u->live.ctrl_command),
        toggle_row("Forward media keys", "Send playback and volume keys to your iPhone.",
                   PH_WORKSPACE_MEDIA, u->live.media_keys),
        toggle_row("Release input when switching apps",
                   "Release held keys and mouse buttons when Porthole loses focus.",
                   PH_WORKSPACE_RELEASE, u->live.release_blur)
    };
    y = setting_group(u, x, y, width, "KEYBOARD", keyboard, SDL_arraysize(keyboard));
    const char *notice = verification_required(u)
                            ? "In Porthole on your iPhone, open Mouse and keyboard and tap Verify Bluetooth."
                        : u->live.paused
                            ? "Enable PC controls to use this PC’s mouse and keyboard with your iPhone."
                        : u->live.mirroring
                            ? "Control your iPhone from the shared screen or toolbar. Settings input stays on this PC."
                        : !u->live.phone_controls_managed || u->live.phone_controls_screen_only
                            ? "PC controls activate when screen sharing starts."
                            : "PC controls are available while your iPhone is connected.";
    y = text(u, notice, x, y, width, 11, true, u->muted);
    return text(u, "File transfers and clipboard sync remain available independently.",
                x, y + 2, width, 11, false, u->muted);
}

static int transfer_settings(ph_workspace *u, int x, int y, int width)
{
    setting_row storage = button_row("Save received files", "Files shared from your iPhone are saved here.",
                                     u->live.folder[0] ? u->live.folder : "Downloads",
                                     PH_WORKSPACE_FOLDER, true);
    y = setting_group(u, x, y, width, "FILE STORAGE", &storage, 1);
    setting_row clipboard[] = {
        toggle_row("Clipboard sync", "Manage synchronization permissions in the iPhone app.",
                   UI_SYNC, u->sync),
        toggle_row("PC to iPhone", "Send clipboard updates to your iPhone.", UI_TO_PHONE, u->to_phone),
        toggle_row("iPhone to PC", "Receive clipboard updates from your iPhone.", UI_TO_PC, u->to_pc)
    };
    y = setting_group(u, x, y, width, "CLIPBOARD", clipboard, SDL_arraysize(clipboard));
    setting_row history[] = {
        button_row("File history", NULL, "Open file history", UI_HISTORY, true),
        button_row("Clipboard history", NULL, "Open clipboard history", PH_WORKSPACE_CLIPBOARD_OPEN, true),
        button_row("Clear clipboard history", "Remove saved clipboard items from this PC.",
                   "Clear clipboard history", UI_CLEAR_ALL_CLIPBOARD, true)
    };
    return setting_group(u, x, y, width, "HISTORY", history, SDL_arraysize(history));
}

static int notification_settings(ph_workspace *u, int x, int y, int width)
{
    setting_row notifications = toggle_row("Transfer notifications",
                                            "Notify me when files are received while Porthole is minimized.",
                                            UI_NOTIFY, u->notifications);
    y = setting_group(u, x, y, width, "DESKTOP NOTIFICATIONS", &notifications, 1);
    setting_row behavior = value_row("Notification action",
                                     "Select a received-file notification to show the saved files.",
                                     "Show in folder");
    y = setting_group(u, x, y, width, "BEHAVIOR", &behavior, 1);
#ifdef _WIN32
    y = text(u, "Windows notification settings and Do not disturb can suppress notifications.",
             x, y, width, 11, false, u->muted) + 2;
#endif
    return text(u, "The bell in the sharing toolbar opens your iPhone’s Notification Center.",
                x, y, width, 11, false, u->muted);
}

static int statistics_settings(ph_workspace *u, int x, int y, int width)
{
    setting_row session[] = {
        value_row("Device", NULL, u->live.phone[0] ? u->live.phone : "—"),
        value_row("Connection", NULL, u->live.connected ? "Wi-Fi"
                                     : u->live.mirroring ? "Screen connected" : "Disconnected"),
        value_row("Screen sharing", NULL, u->live.mirroring ? "Active" : "Inactive"),
        value_row("Bluetooth input", NULL, u->live.controls_connected ? "Active" : "Inactive")
    };
    y = setting_group(u, x, y, width, "SESSION", session, SDL_arraysize(session));
    const ph_metrics_snapshot *metrics = &u->live.metrics;
    if (!u->live.mirroring && !u->live.controls_connected)
        return y;
    char fps[24] = "—", decode[24] = "—", present[24] = "—", input[24] = "—", ack[24] = "—";
    if (u->live.mirroring && metrics->fps_known)
        snprintf(fps, sizeof fps, "%.1f fps", metrics->fps);
    if (u->live.mirroring && metrics->receive_to_decode.known)
        snprintf(decode, sizeof decode, "%.1f ms", metrics->receive_to_decode.mean_ms);
    if (u->live.mirroring && metrics->receive_to_present.known)
        snprintf(present, sizeof present, "%.1f ms", metrics->receive_to_present.mean_ms);
    if (u->live.controls_connected && metrics->input_handoff.known)
        snprintf(input, sizeof input, "%.1f ms", metrics->input_handoff.mean_ms);
    if (u->live.controls_connected && metrics->helper_ack.known)
        snprintf(ack, sizeof ack, "%.1f ms", metrics->helper_ack.mean_ms);
    setting_row measured[] = {
        value_row("Video frame rate", NULL, fps),
        value_row("Receive to decode", NULL, decode),
        value_row("Receive to display", NULL, present),
        value_row("Input handoff", NULL, input),
        value_row("Bluetooth helper acknowledgement", NULL, ack)
    };
    y = setting_group(u, x, y, width, "MEASUREMENTS", measured, SDL_arraysize(measured));
    return text(u, "Times measure processing on this PC.", x, y, width, 11, false, u->muted);
}

static const char *const setting_sections[] = {
    "General", "Screen and audio", "Controls", "Transfers", "Notifications",
    "Help and shortcuts", "Connection statistics", "Devices", "About"
};

static void settings_navigation(ph_workspace *u, SDL_Rect box, bool compact, bool menu)
{
    const ph_control_icon icons[] = {PH_ICON_SUN, PH_ICON_PHONE, PH_ICON_MOUSE, PH_ICON_ARROWS,
                                     PH_ICON_BELL, PH_ICON_HELP, PH_ICON_STATS, PH_ICON_DEVICES,
                                     PH_ICON_INFO};
    rect(u, box, u->surface);
    int pad = compact ? 10 : 15, top = box.y + (compact ? 15 : 18);
    bool footer = !menu && box.h > 400;
    int bottom = box.y + box.h - (footer ? 41 : 10);
    int step = SDL_min(41, (bottom - top) / (int)SDL_arraysize(setting_sections));
    for (size_t i = 0; i < SDL_arraysize(setting_sections); i++) {
        SDL_Rect row = {box.x + pad, top + (int)i * step, box.w - 2 * pad, step - 3};
        if (row.h < 20 || row.y + row.h > bottom)
            break;
        bool selected = u->section == (int)i;
        if (selected)
            rounded(u, row, 7, u->selected_bg);
        SDL_Color ink = selected ? u->ink : u->muted;
        int glyph = compact ? 15 : 17;
        ph_icon_draw(u->renderer, icons[i],
                     (SDL_Rect){row.x + 12, row.y + (row.h - glyph) / 2, glyph, glyph}, ink);
        int label_x = row.x + (compact ? 35 : 39);
        fitted_label(u, setting_sections[i], label_x, row.y + (row.h - 18) / 2,
                     row.x + row.w - label_x - 6, compact ? 11 : 12, selected, ink);
        hit(u, row, UI_SECTION + (int)i, 0, setting_sections[i]);
    }
    if (footer)
        text(u, "Porthole " PORTHOLE_VERSION " · GPL-3.0", box.x + 27, box.y + box.h - 31,
             box.w - 40, 10, false, u->muted);
}

static int device_settings(ph_workspace *u, int x, int y, int width)
{
    if (u->sessions.request_id[0]) {
        y = text(u, u->sessions.request_name, x, y, width, 17, true, u->ink) + 8;
        y = text(u, "Screen Mirroring access requested", x, y, width, 12, false, u->muted) + 12;
        button(u, (SDL_Rect){x, y, 108, 34}, "Review PIN", UI_REVIEW, 0, true, true);
        button(u, (SDL_Rect){x + 118, y, 90, 34}, "Reject", UI_REJECT, 0, false, true);
        y += 54;
    }
    bool registry = false;
    for (int i = 0; i < u->sessions.count; i++)
        registry |= u->sessions.devices[i].companion;
    if (u->live.address[0]) {
        button(u, (SDL_Rect){x, y, 118, 32}, registry || u->live.paired ? "Add iPhone" : "Pair iPhone",
               PH_WORKSPACE_ADD_PHONE, 0, true, true);
        y += 44;
        if (registry)
            y = text(u, "Pair another iPhone and switch between saved devices here.",
                     x, y, width, 12, false, u->muted) + 14;
    } else {
        y = text(u, pairing_recovery(u), x, y, width, 13, false, u->muted) + 12;
        button(u, (SDL_Rect){x, y, 118, 32}, "Try again", PH_WORKSPACE_RETRY_PAIRING, 0, true, true);
        y += 46;
    }
    if (!registry && u->live.paired)
        y = device_row(u, x, y, width, 0);
    for (int i = 0; i < u->sessions.count; i++)
        if (u->sessions.devices[i].companion)
            y = device_row(u, x, y, width, i + 1);
    setting_row approval = toggle_row("Require local approval",
                                       "Verify a PIN on this PC before admitting a new Screen Mirroring device.",
                                       PH_WORKSPACE_PIN_REQUIRED, u->sessions.pin_required);
    y = setting_group(u, x, y, width, "SCREEN MIRRORING", &approval, 1);
    for (int i = 0; i < u->sessions.count; i++)
        if (!u->sessions.devices[i].companion)
            y = device_row(u, x, y, width, i + 1);
    return y;
}

static void settings(ph_workspace *u)
{
    SDL_Rect b = u->rect;
    bool inline_settings = u->live.mirroring;
    bool narrow = b.w < (inline_settings ? 510 : 600);
    int nav = narrow ? 0 : inline_settings ? 180 : 224;
    int pad = narrow ? 16 : inline_settings ? 23 : 44;
    int width = SDL_min(b.w - nav - 2 * pad, 860);
    int x = b.x + nav + pad;
    int top = b.y + 47, bottom = b.y + b.h - (inline_settings ? 34 : 0);
    rect(u, (SDL_Rect){b.x, b.y, b.w, 47}, u->surface);
    line(u, b.x, top - 1, b.w);
    ph_text_draw(u->renderer, "Settings", b.x + 19, b.y + 10, 140,
                 PH_TEXT(17, PH_FONT_SEMIBOLD), u->ink);
    close_button(u, b.x + b.w - 49, b.y + 10, PH_WORKSPACE_CLOSE);
    if (narrow) {
        ph_icon_draw(u->renderer, PH_ICON_CHEVRON,
                     (SDL_Rect){b.x + 91, b.y + 16, 13, 13}, u->muted);
        hit(u, (SDL_Rect){b.x + 10, b.y + 6, 110, 34}, UI_SECTION_MENU, 0, "Settings menu");
    }
    if (!narrow || u->section_menu) {
        SDL_Rect sidebar = {b.x, top, narrow ? b.w : nav, bottom - top};
        settings_navigation(u, sidebar, inline_settings, narrow);
        if (!narrow)
            rect(u, (SDL_Rect){b.x + nav - 1, top, 1, bottom - top}, u->border);
    }
    if (!narrow || !u->section_menu) {
        SDL_Rect clip = {b.x + nav, top, b.w - nav, bottom - top};
        SDL_RenderSetClipRect(u->renderer, &clip);
        int y = top + (inline_settings ? 22 : 30) - u->scroll;
        const char *titles[] = {"General", "Screen and audio", "Mouse and keyboard", "Transfers",
                                "Notifications", "Help and shortcuts", "Connection statistics",
                                "Devices", "About Porthole"};
        const char *subtitles[] = {"Application preferences", "Screen-sharing preferences", NULL,
                                   "File storage and clipboard preferences", "Porthole alerts on this PC",
                                   "Guides for connecting, sharing, and controlling your iPhone.",
                                   "Current connection details", NULL, "Version and license"};
        int section = SDL_clamp(u->section, 0, PH_SETTINGS_COUNT - 1);
        y = settings_heading(u, x, y, width, inline_settings, titles[section], subtitles[section]);
        switch (section) {
        case 0: y = general_settings(u, x, y, width, inline_settings); break;
        case 1: y = screen_settings(u, x, y, width); break;
        case PH_SETTINGS_CONTROLS:
            status_dot(u, x, y - 17, u->live.controls);
            text(u, controls_status(u), x + 14, y - 22, width - 14, 12, false, u->muted);
            y = control_settings(u, x, y + 16, width);
            break;
        case 3: y = transfer_settings(u, x, y, width); break;
        case 4: y = notification_settings(u, x, y, width); break;
        case PH_SETTINGS_HELP: y = help_settings(u, x, y - 1, width); break;
        case 6: y = statistics_settings(u, x, y, width); break;
        case PH_SETTINGS_DEVICES: y = device_settings(u, x, y, width); break;
        case PH_SETTINGS_ABOUT: {
            setting_row about[] = {value_row("Porthole", NULL, PORTHOLE_VERSION),
                                   value_row("License", NULL, "GPL-3.0")};
            y = setting_group(u, x, y, width, "APPLICATION", about, SDL_arraysize(about));
            break;
        }
        }
        SDL_RenderSetClipRect(u->renderer, NULL);
        int visible = bottom - top;
        u->scroll_limit = SDL_max(0, y + u->scroll + 16 - bottom);
        if (u->scroll_limit && visible > 0) {
            int thumb = SDL_max(30, visible * visible / (u->scroll_limit + visible));
            int dy = (visible - thumb) * u->scroll / u->scroll_limit;
            rounded(u, (SDL_Rect){b.x + b.w - 7, top + dy, 3, thumb}, 2, u->border);
        }
    }
    if (inline_settings) {
        rect(u, (SDL_Rect){b.x, bottom, b.w, 34}, u->surface);
        line(u, b.x, bottom, b.w);
        status_dot(u, b.x + 17, bottom + 15, true);
        fitted_label(u, "Screen sharing continues", b.x + 30, bottom + 10, b.w / 2 - 30,
                     10, false, u->muted);
        const char *controls = verification_required(u) ? "Bluetooth verification required"
                               : u->live.controls && u->section == PH_SETTINGS_HELP
                                   ? "PC controls paused in Help"
                               : u->live.controls ? "Mouse active over iPhone preview"
                               : u->live.paused ? "PC controls disabled"
                                                : "PC controls inactive";
        int controls_width = ph_text_width(controls, PH_TEXT(10, PH_FONT_REGULAR));
        int controls_space = SDL_min(controls_width + 2, b.w / 2 - 16);
        fitted_label(u, controls, b.x + b.w - 16 - controls_space, bottom + 10,
                     controls_space, 10, false, u->muted);
    }
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
    SDL_RenderSetClipRect(u->renderer, NULL);
    rect(u, b, view == PH_WORKSPACE_FILES ? u->surface : u->background);
    if (view == PH_WORKSPACE_HOME) {
        if (!u->live.paired)
            SDL_RenderSetClipRect(u->renderer, &b);
        home(u);
        SDL_RenderSetClipRect(u->renderer, NULL);
    } else if (view == PH_WORKSPACE_SETTINGS)
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
        u->date_menu = false;
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
    if (a == UI_HELP_GUIDE && h.id < SDL_arraysize(help_guides)) {
        u->expanded_guides ^= 1u << h.id;
        return;
    }
    if (a == UI_SIGN_IN) {
        if (ph_config_set_launch_at_sign_in(!u->launch_at_sign_in))
            u->launch_at_sign_in = !u->launch_at_sign_in;
        else
            snprintf(u->error, sizeof u->error, "Could not change the launch at sign-in setting.");
        return;
    }
    if (a == UI_KEEP_ASPECT) {
        u->keep_aspect = !u->keep_aspect;
        save_preferences(u);
        return;
    }
    if (a >= UI_SECTION && a < UI_SECTION + PH_SETTINGS_COUNT) {
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
        if (!h.id) {
            u->date_menu = !u->date_menu;
            return;
        }
        u->date_range = (int)h.id - 1;
        u->date_menu = false;
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
            if (ph_history_remove_clip(u->history, h.id))
                clear_previews(u);
            else
                history_save_error(u);
        } else {
            u->confirm = 3;
        }
        return;
    }
    if (a == UI_CLIP_DELETE) {
        if (ph_history_remove_clips(u->history, u->clip_selected, u->clip_selected_n)) {
            clear_previews(u);
            u->clip_selected_n = 0;
        } else
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
            clear_previews(u);
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
        (u->confirm || u->date_menu || u->history_open || u->view == PH_WORKSPACE_SETTINGS ||
         u->view == PH_WORKSPACE_CLIPBOARD || u->focused.action)) {
        if (u->confirm)
            u->confirm = 0;
        else if (u->date_menu)
            u->date_menu = false;
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
#if SDL_VERSION_ATLEAST(2, 26, 0)
        x = event->wheel.mouseX;
        y = event->wheel.mouseY;
#else
        SDL_GetMouseState(&x, &y);
#endif
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
