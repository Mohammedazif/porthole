#include "files/history.h"
#include "core/upath.h"
#include "temp_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int main(void)
{
    char dir[1024], database[1100], path[1100], renamed[1100];
    CHECK(ph_test_temp_dir(dir, sizeof dir));
    snprintf(database, sizeof database, "%s/history.bin", dir);
    snprintf(path, sizeof path, "%s/Résumé.pdf", dir);
    snprintf(renamed, sizeof renamed, "%s/Révision.pdf", dir);
    FILE *f = ph_fopen(path, "wb");
    CHECK(f);
    CHECK(fwrite("sample", 1, 6, f) == 6 && fclose(f) == 0);
    ph_history *h = ph_history_open(database);
    CHECK(h);
    ph_file_entry e = {.bytes = 6, .confirmed = 3, .incoming = true, .state = PH_TRANSFER_ACTIVE};
    snprintf(e.digest, sizeof e.digest,
             "af2bdbe1aa9b6ec1e2ade1d694f41fc71a831d0268e9891562113d8a62add1bf");
    CHECK(strlen(path) < sizeof e.path);
    memcpy(e.path, path, strlen(path) + 1);
    snprintf(e.name, sizeof e.name, "Résumé.pdf");
    snprintf(e.device, sizeof e.device, "Alex’s iPhone");
    uint64_t id = ph_history_add_file(h, &e);
    CHECK(id);
    CHECK(ph_history_file_availability(ph_history_file(h, 0)) == PH_FILE_PRESENT);
    CHECK(ph_history_add_clip(h, 'T', "hello", 5, true));
    CHECK(ph_history_add_clip(h, 'T', "hello", 5, false));
    CHECK(ph_history_clip_count(h) == 1); /* remote application does not echo duplicate history */
    const unsigned char png[] = {137, 80, 78, 71, 13, 10, 26, 10, 0};
    CHECK(ph_history_add_clip(h, 'P', png, sizeof png, false));
    ph_history_close(h);
    h = ph_history_open(database);
    CHECK(h);
    CHECK(ph_history_file_count(h) == 1 && ph_history_clip_count(h) == 2);
    CHECK(ph_history_file(h, 0)->state == PH_TRANSFER_PAUSED);
    CHECK(ph_history_file(h, 0)->confirmed == 3);
    CHECK(!strcmp(ph_history_file(h, 0)->device, "Alex’s iPhone"));
    CHECK(!strcmp(ph_history_file(h, 0)->digest, e.digest));
    CHECK(ph_history_match_file(ph_history_file(h, 0), path));
    CHECK(ph_rename(path, renamed) == 0);
    CHECK(ph_history_match_file(ph_history_file(h, 0), renamed));
    f = ph_fopen(path, "wb");
    CHECK(f && fwrite("target", 1, 6, f) == 6 && fclose(f) == 0);
    CHECK(!ph_history_match_file(ph_history_file(h, 0), path));
    CHECK(ph_remove(path) == 0);
    CHECK(ph_rename(renamed, path) == 0);
    CHECK(!memcmp(ph_history_clip(h, 0)->data, png, sizeof png));
    CHECK(ph_history_clip(h, 0)->kind == 'P' && ph_history_clip(h, 1)->incoming);
    CHECK(!ph_history_update_file(h, id, PH_TRANSFER_COMPLETE, 7, NULL));
    CHECK(ph_history_update_file(h, id, PH_TRANSFER_COMPLETE, 6, NULL));
    CHECK(ph_remove(path) == 0);
    CHECK(ph_history_file_availability(ph_history_file(h, 0)) == PH_FILE_MISSING);
    char moved[1100];
    snprintf(moved, sizeof moved, "%s-moved", dir);
    CHECK(ph_rename(dir, moved) == 0);
    CHECK(!ph_history_clear_clips(h));
    CHECK(ph_history_clip_count(h) == 2);
    CHECK(!ph_history_remove_file(h, id));
    CHECK(ph_history_file_count(h) == 1);
    CHECK(ph_rename(moved, dir) == 0);
    CHECK(ph_history_remove_clip(h, ph_history_clip(h, 0)->id));
    CHECK(ph_history_clip_count(h) == 1);
    CHECK(ph_history_clear_files(h) && ph_history_clear_clips(h));
    ph_history_close(h);
    h = ph_history_open(database);
    CHECK(h);
    CHECK(!ph_history_file_count(h) && !ph_history_clip_count(h));
    for (int i = 0; i < 105; i++) {
        char text[16];
        int n = snprintf(text, sizeof text, "item %d", i);
        CHECK(ph_history_add_clip(h, 'T', text, (size_t)n, false));
    }
    CHECK(ph_history_clip_count(h) == 100);
    CHECK(!strcmp((const char *)ph_history_clip(h, 0)->data, "item 104"));
    ph_history_close(h);
    f = ph_fopen(database, "wb");
    CHECK(f);
    CHECK(fwrite("corrupt", 1, 7, f) == 7 && fclose(f) == 0);
    h = ph_history_open(database);
    CHECK(h && !ph_history_clip_count(h));
    ph_history_close(h);
    CHECK(ph_file_size(database) == 7); /* a damaged file is preserved */
    CHECK(ph_remove(database) == 0 && ph_rmdir(dir) == 0);
    return 0;
}
