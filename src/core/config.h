#ifndef PORTHOLE_CORE_CONFIG_H
#define PORTHOLE_CORE_CONFIG_H

/* Per-user settings directory and small files inside it.
 * Linux: $XDG_CONFIG_HOME/porthole (default ~/.config/porthole); Windows: %APPDATA%\Porthole. */

#include <stdbool.h>
#include <stddef.h>

/* Writes the directory path (created if missing). Returns false if it can't be created. */
bool ph_config_dir(char *out, size_t out_len);

/* Writes "<config dir>/<name>". Returns false if the path doesn't fit or the dir is unusable. */
bool ph_config_path(const char *name, char *out, size_t out_len);

/* Reads the first line of a config file (without the newline). False if missing/empty. */
bool ph_config_read_line(const char *name, char *out, size_t out_len);

/* Replaces a config file with one line of text. */
bool ph_config_write_line(const char *name, const char *line);

bool ph_config_launch_at_sign_in(void);
bool ph_config_set_launch_at_sign_in(bool enabled);
bool ph_config_launch_managed(void);

#endif
