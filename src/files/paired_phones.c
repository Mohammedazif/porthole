#include "files/paired_phones.h"

#include "core/config.h"
#include "core/upath.h"

#include <openssl/evp.h>

bool ph_paired_phone_identity(const char *key, char out[65])
{
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[32];
    unsigned length = 0;
    if (strlen(key) != 32 || strspn(key, digits) != 32 ||
        EVP_Digest(key, 32, digest, &length, EVP_sha256(), NULL) != 1 ||
        length != sizeof digest)
        return false;
    for (size_t i = 0; i < sizeof digest; i++) {
        out[2 * i] = digits[digest[i] >> 4];
        out[2 * i + 1] = digits[digest[i] & 15];
    }
    out[64] = 0;
    return true;
}

int ph_paired_phones_find(const ph_paired_phones *phones, const char *id)
{
    for (int i = 0; id && i < phones->count; i++)
        if (!strcmp(phones->items[i].id, id))
            return i;
    return -1;
}

const ph_paired_phone *ph_paired_phones_selected(const ph_paired_phones *phones)
{
    return phones->selected >= 0 && phones->selected < phones->count
               ? &phones->items[phones->selected]
               : NULL;
}

bool ph_paired_phones_is_revoked(const ph_paired_phones *phones, const char *id)
{
    for (int i = 0; i < phones->revoked_count; i++)
        if (!strcmp(phones->revoked[i], id))
            return true;
    return false;
}

void ph_paired_phones_revoke(ph_paired_phones *phones, const char *id)
{
    if (ph_paired_phones_is_revoked(phones, id))
        return;
    if (phones->revoked_count == PH_REVOKED_PHONE_LIMIT) {
        memmove(phones->revoked, phones->revoked + 1,
                sizeof phones->revoked - sizeof phones->revoked[0]);
        phones->revoked_count--;
    }
    memcpy(phones->revoked[phones->revoked_count++], id, 65);
}

bool ph_paired_phones_save(const ph_paired_phones *phones)
{
    const ph_paired_phone *selected = ph_paired_phones_selected(phones);
    char data[4096];
    int n = snprintf(data, sizeof data, "PORTHOLE1 %s", selected ? selected->id : "-");
    for (int i = 0; i < phones->revoked_count; i++) {
        int used = snprintf(data + n, sizeof data - (size_t)n, "\nREVOKED %s", phones->revoked[i]);
        if (used < 0 || (size_t)used >= sizeof data - (size_t)n)
            return false;
        n += used;
    }
    for (int i = 0; i < phones->count; i++) {
        const ph_paired_phone *phone = &phones->items[i];
        int used = snprintf(data + n, sizeof data - (size_t)n, "\n%s\t%s\t%s\t%s",
                            phone->key, phone->seed, phone->remote, phone->alias);
        if (used < 0 || (size_t)used >= sizeof data - (size_t)n)
            return false;
        n += used;
    }
    return ph_config_write_line("companion-pairings", data);
}

static void migrate_legacy(ph_paired_phones *phones)
{
    char record[80], token[16] = "", code[16], key[33];
    if (!ph_config_read_line("pull-token", token, sizeof token) ||
        !ph_config_read_line("paired-credential", record, sizeof record) ||
        sscanf(record, "%8[0-9A-Fa-f] %32[0-9a-f]", code, key) != 2 || strcmp(token, code) ||
        strlen(code) != 8)
        return;
    ph_paired_phone *phone = &phones->items[0];
    if (!ph_paired_phone_identity(key, phone->id))
        return;
    memcpy(phone->key, key, 33);
    memcpy(phone->seed, code, 9);
    ph_config_read_line("companion-name", phone->alias, sizeof phone->alias);
    phone->alias[strcspn(phone->alias, "\t")] = 0;
    phones->count = 1;
    phones->selected = 0;
    ph_paired_phones_save(phones);
}

void ph_paired_phones_load(ph_paired_phones *phones)
{
    *phones = (ph_paired_phones){.selected = -1};
    char path[1100], line[256], selected[65] = "";
    if (!ph_config_path("companion-pairings", path, sizeof path))
        return;
    FILE *file = ph_fopen(path, "rb");
    if (!file) {
        if (!ph_path_exists(path))
            migrate_legacy(phones);
        return;
    }
    if (fgets(line, sizeof line, file) && !strncmp(line, "PORTHOLE1 ", 10)) {
        sscanf(line + 10, "%64[0-9a-f]", selected);
        while (fgets(line, sizeof line, file)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "REVOKED ", 8)) {
                if (strlen(line + 8) == 64 && strspn(line + 8, "0123456789abcdef") == 64)
                    ph_paired_phones_revoke(phones, line + 8);
                continue;
            }
            if (phones->count == PH_PAIRED_PHONE_LIMIT)
                continue;
            char *seed = strchr(line, '\t');
            char *remote = seed ? strchr(seed + 1, '\t') : NULL;
            char *alias = remote ? strchr(remote + 1, '\t') : NULL;
            if (!alias)
                continue;
            *seed++ = *remote++ = *alias++ = 0;
            ph_paired_phone *phone = &phones->items[phones->count];
            if (strlen(seed) != 8 || strspn(seed, "0123456789abcdefABCDEF") != 8 ||
                strlen(remote) >= sizeof phone->remote || strlen(alias) >= sizeof phone->alias ||
                !ph_paired_phone_identity(line, phone->id) ||
                ph_paired_phones_find(phones, phone->id) >= 0 ||
                ph_paired_phones_is_revoked(phones, phone->id))
                continue;
            memcpy(phone->key, line, sizeof phone->key);
            memcpy(phone->seed, seed, sizeof phone->seed);
            snprintf(phone->remote, sizeof phone->remote, "%s", remote);
            snprintf(phone->alias, sizeof phone->alias, "%s", alias);
            phones->count++;
        }
        phones->selected = ph_paired_phones_find(phones, selected);
    }
    fclose(file);
}
