#include "files/history.h"
#include "files/clip.h"
#include "files/digest.h"
#include "core/config.h"
#include "core/upath.h"
#include <errno.h>
#include <openssl/rand.h>
#include <time.h>
#ifndef _WIN32
#include <dirent.h>
#endif

#define FILE_LIMIT 100000u
#define CLIP_LIMIT 100u
#define CLIP_TOTAL_MAX (32u * 1024u * 1024u)
struct ph_history {
    char path[1200];
    ph_file_entry *files;
    ph_clip_entry clips[CLIP_LIMIT];
    size_t files_n, clips_n, clip_bytes;
    uint64_t next;
};
static bool number(FILE *f, uint64_t *value, bool write)
{
    unsigned char b[8];
    if (write) {
        for (int i = 0; i < 8; i++)
            b[i] = (unsigned char)(*value >> (8 * i));
        return fwrite(b, 1, 8, f) == 8;
    }
    if (fread(b, 1, 8, f) != 8)
        return false;
    *value = 0;
    for (int i = 0; i < 8; i++)
        *value |= (uint64_t)b[i] << (8 * i);
    return true;
}
static bool string(FILE *f, char *s, size_t cap, bool write)
{
    uint64_t n = write ? strlen(s) : 0;
    if (!number(f, &n, write) || n >= cap)
        return false;
    if (write)
        return fwrite(s, 1, (size_t)n, f) == n;
    if (fread(s, 1, (size_t)n, f) != n || memchr(s, 0, (size_t)n))
        return false;
    s[n] = 0;
    return true;
}
static bool file_record(FILE *f, ph_file_entry *entry, bool write, unsigned version)
{
    uint64_t stamp = (uint64_t)entry->time;
    uint64_t flags = (uint64_t)entry->state * 2u + (entry->incoming ? 1u : 0u);
    if (!number(f, &entry->id, write) || !number(f, &entry->bytes, write) ||
        !number(f, &entry->confirmed, write) || !number(f, &stamp, write) ||
        !number(f, &flags, write) || !string(f, entry->device, sizeof entry->device, write) ||
        !string(f, entry->name, sizeof entry->name, write) ||
        !string(f, entry->path, sizeof entry->path, write) ||
        (version >= 2 && !string(f, entry->transfer_id, sizeof entry->transfer_id, write)) ||
        (version >= 3 && (!string(f, entry->digest, sizeof entry->digest, write) ||
                          !string(f, entry->resume_key, sizeof entry->resume_key, write))))
        return false;
    if (flags > PH_TRANSFER_CHANGED * 2u + 1u || entry->confirmed > entry->bytes)
        return false;
    entry->time = (int64_t)stamp;
    entry->state = (ph_transfer_state)(flags / 2u);
    entry->incoming = (flags & 1u) != 0;
    return entry->id != 0;
}
static void clear_memory(ph_history *h)
{
    for (size_t i = 0; i < h->clips_n; i++)
        free(h->clips[i].data);
    free(h->files);
    h->files = NULL;
    h->files_n = h->clips_n = h->clip_bytes = 0;
}
ph_history *ph_history_open(const char *path)
{
    ph_history *h = calloc(1, sizeof *h);
    FILE *f;
    unsigned char magic[8];
    uint64_t files = 0, clips = 0;
    if (!h)
        return NULL;
    h->next = 1;
    if (!path || !path[0])
        return h;
    if (strlen(path) >= sizeof h->path) {
        free(h);
        return NULL;
    }
    snprintf(h->path, sizeof h->path, "%s", path);
    f = ph_fopen(path, "rb");
    if (!f)
        return h;
    if (fread(magic, 1, 8, f) != 8 ||
        memcmp(magic, "PHHIST", 6) || magic[6] < '1' || magic[6] > '3' || magic[7] != '\n' ||
        !number(f, &h->next, false) || !h->next || !number(f, &files, false) ||
        !number(f, &clips, false) || files > FILE_LIMIT || clips > CLIP_LIMIT)
        goto corrupt;
    if (files) {
        h->files = calloc((size_t)files, sizeof *h->files);
        if (!h->files)
            goto corrupt;
    }
    for (size_t i = 0; i < files; i++) {
        if (!file_record(f, &h->files[i], false, (unsigned)(magic[6] - '0')) ||
            h->files[i].id >= h->next)
            goto corrupt;
        h->files_n++;
        /* A process cannot leave a file marked as actively moving after restart. */
        if (h->files[i].state == PH_TRANSFER_ACTIVE || h->files[i].state == PH_TRANSFER_QUEUED)
            h->files[i].state = PH_TRANSFER_PAUSED;
    }
    for (size_t i = 0; i < clips; i++) {
        ph_clip_entry c = {0};
        uint64_t flags = 0, stamp = 0, size = 0;
        if (!number(f, &c.id, false) || !c.id || c.id >= h->next || !number(f, &stamp, false) ||
            !number(f, &flags, false) || !number(f, &size, false) || (flags & ~511u) || !size ||
            ((char)(flags & 255u) != 'T' && (char)(flags & 255u) != 'P') ||
            size > ((char)flags == 'T' ? PH_CLIP_TEXT_MAX : PH_CLIP_IMAGE_MAX) ||
            size > CLIP_TOTAL_MAX - h->clip_bytes)
            goto corrupt;
        c.data = malloc((size_t)size + 1);
        if (!c.data)
            goto corrupt;
        if (fread(c.data, 1, (size_t)size, f) != size) {
            free(c.data);
            goto corrupt;
        }
        c.data[size] = 0;
        c.size = (size_t)size;
        c.kind = (char)(flags & 255u);
        c.time = (int64_t)stamp;
        c.incoming = (flags & 256u) != 0;
        h->clips[h->clips_n++] = c;
        h->clip_bytes += c.size;
    }
    if (fgetc(f) != EOF)
        goto corrupt;
    fclose(f);
    return h;
corrupt:
    fclose(f);
    clear_memory(h);
    h->next = 1;
    /* Preserve the damaged file; opening history never replaces it. */
    h->path[0] = 0;
    return h;
}
bool ph_history_save(ph_history *h)
{
    char part[1230];
    FILE *f;
    bool ok;
    uint64_t files, clips;
    if (!h)
        return false;
    if (!h->path[0])
        return true;
    unsigned char nonce[8];
    if (RAND_bytes(nonce, sizeof nonce) != 1)
        return false;
    int len = snprintf(part, sizeof part, "%s.%02x%02x%02x%02x%02x%02x%02x%02x.tmp", h->path,
                       nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
    if (len < 0 || (size_t)len >= sizeof part)
        return false;
    f = ph_fopen(part, "wbx");
    if (!f)
        return false;
#ifndef _WIN32
    if (fchmod(fileno(f), 0600) != 0) {
        fclose(f);
        ph_remove(part);
        return false;
    }
#endif
    files = h->files_n;
    clips = h->clips_n;
    ok = fwrite("PHHIST3\n", 1, 8, f) == 8 && number(f, &h->next, true) &&
         number(f, &files, true) && number(f, &clips, true);
    for (size_t i = 0; ok && i < h->files_n; i++)
        ok = file_record(f, &h->files[i], true, 3);
    for (size_t i = 0; ok && i < h->clips_n; i++) {
        ph_clip_entry *c = &h->clips[i];
        uint64_t stamp = (uint64_t)c->time,
                 flags = (unsigned char)c->kind + (c->incoming ? 256u : 0u), size = c->size;
        ok = number(f, &c->id, true) && number(f, &stamp, true) && number(f, &flags, true) &&
             number(f, &size, true) && fwrite(c->data, 1, c->size, f) == c->size;
    }
    ok = ok && ph_flush_file(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok || ph_rename(part, h->path) != 0) {
        ph_remove(part);
        return false;
    }
    return true;
}
void ph_history_close(ph_history *h)
{
    if (h) {
        clear_memory(h);
        free(h);
    }
}
size_t ph_history_file_count(const ph_history *h)
{
    return h ? h->files_n : 0;
}
size_t ph_history_clip_count(const ph_history *h)
{
    return h ? h->clips_n : 0;
}
const ph_file_entry *ph_history_file(const ph_history *h, size_t i)
{
    return h && i < h->files_n ? &h->files[h->files_n - 1 - i] : NULL;
}
const ph_clip_entry *ph_history_clip(const ph_history *h, size_t i)
{
    return h && i < h->clips_n ? &h->clips[i] : NULL;
}

static void terminate_file_entry(ph_file_entry *f)
{
    f->name[sizeof f->name - 1] = f->path[sizeof f->path - 1] = f->device[sizeof f->device - 1] = 0;
    f->transfer_id[sizeof f->transfer_id - 1] = f->digest[sizeof f->digest - 1] =
        f->resume_key[sizeof f->resume_key - 1] = 0;
}

uint64_t ph_history_add_file(ph_history *h, const ph_file_entry *entry)
{
    ph_file_entry *grown;
    if (!h || !entry || h->files_n >= FILE_LIMIT || entry->state > PH_TRANSFER_CHANGED ||
        entry->confirmed > entry->bytes || !h->next || h->next == UINT64_MAX)
        return 0;
    grown = realloc(h->files, (h->files_n + 1) * sizeof *grown);
    if (!grown)
        return 0;
    h->files = grown;
    h->files[h->files_n] = *entry;
    ph_file_entry *f = &h->files[h->files_n++];
    f->id = h->next++;
    if (!f->time)
        f->time = (int64_t)time(NULL);
    terminate_file_entry(f);
    if (ph_history_save(h))
        return f->id;
    h->files_n--;
    h->next--;
    return 0;
}

bool ph_history_replace_file(ph_history *h, uint64_t id, const ph_file_entry *entry)
{
    if (!h || !entry || entry->state > PH_TRANSFER_CHANGED || entry->confirmed > entry->bytes)
        return false;
    for (size_t i = 0; i < h->files_n; i++) {
        ph_file_entry *f = &h->files[i];
        if (f->id != id)
            continue;
        ph_file_entry previous = *f;
        *f = *entry;
        f->id = previous.id;
        f->time = previous.time;
        terminate_file_entry(f);
        if (ph_history_save(h))
            return true;
        *f = previous;
        return false;
    }
    return false;
}

bool ph_history_match_file(const ph_file_entry *entry, const char *path)
{
    if (!entry || !path)
        return false;
    long long bytes = ph_file_size(path);
    if (bytes < 0 || (uint64_t)bytes != entry->bytes)
        return false;
    if (!entry->digest[0])
        return strcmp(entry->name, ph_path_base(path)) == 0;
    char digest[65];
    FILE *file = ph_fopen(path, "rb");
    if (!file)
        return false;
    bool matches = ph_file_digest(file, digest, NULL, NULL) && strcmp(entry->digest, digest) == 0;
    fclose(file);
    return matches;
}
bool ph_history_update_file(ph_history *h, uint64_t id, ph_transfer_state state, uint64_t confirmed,
                            const char *path)
{
    if (!h || state > PH_TRANSFER_CHANGED || (path && strlen(path) >= sizeof h->files[0].path))
        return false;
    for (size_t i = 0; i < h->files_n; i++)
        if (h->files[i].id == id) {
            ph_file_entry *f = &h->files[i];
            if (confirmed > f->bytes)
                return false;
            ph_file_entry previous = *f;
            f->state = state;
            f->confirmed = confirmed;
            if (path)
                snprintf(f->path, sizeof f->path, "%s", path);
            if (ph_history_save(h))
                return true;
            *f = previous;
            return false;
        }
    return false;
}
uint64_t ph_history_add_clip(ph_history *h, char kind, const void *data, size_t size, bool incoming)
{
    if (!h || !data || !size || (kind != 'T' && kind != 'P') ||
        size > (kind == 'T' ? PH_CLIP_TEXT_MAX : PH_CLIP_IMAGE_MAX) || !h->next ||
        h->next == UINT64_MAX)
        return 0;
    if (h->clips_n && h->clips[0].kind == kind && h->clips[0].size == size &&
        memcmp(h->clips[0].data, data, size) == 0)
        return h->clips[0].id;
    unsigned char *copy = malloc(size + 1);
    if (!copy)
        return 0;
    memcpy(copy, data, size);
    copy[size] = 0;
    ph_history next = *h;
    while (next.clips_n >= CLIP_LIMIT || size > CLIP_TOTAL_MAX - next.clip_bytes) {
        next.clip_bytes -= next.clips[--next.clips_n].size;
    }
    memmove(next.clips + 1, next.clips, next.clips_n * sizeof next.clips[0]);
    next.clips[0] = (ph_clip_entry){next.next++, (int64_t)time(NULL), incoming, kind, size, copy};
    next.clips_n++;
    next.clip_bytes += size;
    if (!ph_history_save(&next)) {
        free(copy);
        return 0;
    }
    for (size_t i = next.clips_n - 1; i < h->clips_n; i++)
        free(h->clips[i].data);
    *h = next;
    return h->clips[0].id;
}
bool ph_history_remove_file(ph_history *h, uint64_t id)
{
    if (!h)
        return false;
    for (size_t i = 0; i < h->files_n; i++)
        if (h->files[i].id == id) {
            ph_file_entry previous = h->files[i];
            memmove(h->files + i, h->files + i + 1, (h->files_n - i - 1) * sizeof h->files[0]);
            h->files_n--;
            if (ph_history_save(h))
                return true;
            memmove(h->files + i + 1, h->files + i, (h->files_n - i) * sizeof h->files[0]);
            h->files[i] = previous;
            h->files_n++;
            return false;
        }
    return false;
}
bool ph_history_remove_clips(ph_history *h, const uint64_t *ids, size_t count)
{
    if (!h || !ids || !count)
        return false;
    ph_history next = *h;
    bool removed[CLIP_LIMIT] = {false};
    next.clips_n = 0;
    for (size_t i = 0; i < h->clips_n; i++) {
        for (size_t j = 0; j < count; j++)
            if (h->clips[i].id == ids[j]) {
                removed[i] = true;
                break;
            }
        if (removed[i])
            next.clip_bytes -= h->clips[i].size;
        else
            next.clips[next.clips_n++] = h->clips[i];
    }
    if (next.clips_n == h->clips_n || !ph_history_save(&next))
        return false;
    for (size_t i = 0; i < h->clips_n; i++)
        if (removed[i])
            free(h->clips[i].data);
    *h = next;
    return true;
}
bool ph_history_remove_clip(ph_history *h, uint64_t id)
{
    return ph_history_remove_clips(h, &id, 1);
}
bool ph_history_clear_files(ph_history *h)
{
    if (!h)
        return false;
    ph_history next = *h;
    next.files_n = 0;
    if (!ph_history_save(&next))
        return false;
    *h = next;
    return true;
}
bool ph_history_clear_clips(ph_history *h)
{
    if (!h)
        return false;
    ph_history next = *h;
    next.clips_n = next.clip_bytes = 0;
    if (!ph_history_save(&next))
        return false;
    for (size_t i = 0; i < h->clips_n; i++)
        free(h->clips[i].data);
    *h = next;
    return true;
}

static bool clear_profile_clips(const char *folder, const char *entry, ph_history *current)
{
    if (strncmp(entry, "history-", 8) || strlen(entry) != 44 ||
        strspn(entry + 8, "0123456789abcdef") != 32 || strcmp(entry + 40, ".bin"))
        return true;
    char path[1200];
#ifdef _WIN32
    const char separator = '\\';
#else
    const char separator = '/';
#endif
    int length = snprintf(path, sizeof path, "%s%c%s", folder, separator, entry);
    if (length < 0 || (size_t)length >= sizeof path)
        return false;
    if (!strcmp(path, current->path))
        return true;
    ph_history *store = ph_history_open(path);
    bool ok = store && store->path[0] && (!store->clips_n || ph_history_clear_clips(store));
    ph_history_close(store);
    return ok;
}

bool ph_history_clear_saved_clips(ph_history *current)
{
    if (!current || !ph_history_clear_clips(current))
        return false;
    char folder[1024];
    if (!ph_config_dir(folder, sizeof folder))
        return false;
    bool ok = true;
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s\\history-*.bin", folder);
    wchar_t *wide = ph_wide(pattern);
    if (!wide)
        return false;
    WIN32_FIND_DATAW data;
    HANDLE scan = FindFirstFileW(wide, &data);
    free(wide);
    if (scan == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        char name[512];
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            ph_narrow(data.cFileName, name, sizeof name) && !clear_profile_clips(folder, name, current))
            ok = false;
    } while (FindNextFileW(scan, &data));
    FindClose(scan);
#else
    DIR *scan = opendir(folder);
    if (!scan)
        return false;
    struct dirent *item;
    while ((item = readdir(scan)))
        if (!clear_profile_clips(folder, item->d_name, current))
            ok = false;
    closedir(scan);
#endif
    return ok;
}

ph_file_availability ph_history_file_availability(const ph_file_entry *entry)
{
    char parent[1024], *slash;
    if (!entry || !entry->path[0])
        return PH_FILE_NO_PATH;
    if (ph_file_size(entry->path) >= 0)
        return PH_FILE_PRESENT;
    snprintf(parent, sizeof parent, "%s", entry->path);
    slash = strrchr(parent, '/');
#ifdef _WIN32
    char *back = strrchr(parent, '\\');
    if (back && (!slash || back > slash))
        slash = back;
#endif
    if (!slash)
        return PH_FILE_MISSING;
    if (slash == parent)
        slash[1] = 0;
    else
        *slash = 0;
    return ph_path_exists(parent) ? PH_FILE_MISSING : PH_FILE_LOCATION_UNAVAILABLE;
}
