/*
 * cmd/session_mgr.c - session transcript discovery, loading, and persistence.
 */

#include "session_mgr.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int ensure_dir_exists(const char *dir) {
    char tmp[1024];
    if (!dir || strlen(dir) >= sizeof(tmp)) {
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int puci_cli_get_sessions_dir(char *out_dir, size_t cap) {
    if (!out_dir || cap == 0) {
        return -1;
    }
    struct stat st;
    if (stat(".puci/sessions", &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(out_dir, cap, ".puci/sessions");
        return 0;
    }
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        snprintf(out_dir, cap, "%s/puci/sessions", xdg);
    } else {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(out_dir, cap, "%s/.local/share/puci/sessions", home);
        } else {
            snprintf(out_dir, cap, "/tmp/puci-sessions");
        }
    }
    ensure_dir_exists(out_dir);
    return 0;
}

int puci_cli_get_history_file(char *out_path, size_t cap) {
    if (!out_path || cap == 0) {
        return -1;
    }
    const char *xdg = getenv("XDG_DATA_HOME");
    char dir[1024];
    if (xdg && xdg[0]) {
        snprintf(dir, sizeof(dir), "%s/puci", xdg);
    } else {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(dir, sizeof(dir), "%s/.local/share/puci", home);
        } else {
            snprintf(dir, sizeof(dir), "/tmp");
        }
    }
    ensure_dir_exists(dir);
    snprintf(out_path, cap, "%s/history", dir);
    return 0;
}

int puci_cli_save_session(const puci_session *sess, const char *session_id) {
    if (!sess || !session_id || !session_id[0]) {
        return -1;
    }
    char dir[1024];
    if (puci_cli_get_sessions_dir(dir, sizeof(dir)) != 0) {
        return -1;
    }
    char filepath[2048];
    snprintf(filepath, sizeof(filepath), "%s/%s.json", dir, session_id);
    puci_status st = puci_session_save_file(sess, filepath);
    return (st == PUCI_OK) ? 0 : -1;
}

int puci_cli_load_session(puci_session *sess, const char *id_or_latest, char *out_id, size_t id_cap) {
    if (!sess) {
        return -1;
    }
    char dir[1024];
    if (puci_cli_get_sessions_dir(dir, sizeof(dir)) != 0) {
        return -1;
    }
    char target_id[256];
    target_id[0] = '\0';

    if (!id_or_latest || !id_or_latest[0] || strcmp(id_or_latest, "latest") == 0) {
        /* Find the newest .json file by mtime */
        DIR *d = opendir(dir);
        if (!d) {
            return -1;
        }
        struct dirent *ent;
        time_t newest_time = 0;
        char newest_name[256] = "";

        while ((ent = readdir(d)) != NULL) {
            size_t len = strlen(ent->d_name);
            if (len > 5 && strcmp(ent->d_name + len - 5, ".json") == 0) {
                char full[2048];
                snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
                struct stat st;
                if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                    if (st.st_mtime >= newest_time) {
                        newest_time = st.st_mtime;
                        snprintf(newest_name, sizeof(newest_name), "%.*s", (int)(len - 5), ent->d_name);
                    }
                }
            }
        }
        closedir(d);
        if (!newest_name[0]) {
            return -1; /* No sessions found */
        }
        snprintf(target_id, sizeof(target_id), "%s", newest_name);
    } else {
        snprintf(target_id, sizeof(target_id), "%s", id_or_latest);
        size_t len = strlen(target_id);
        if (len > 5 && strcmp(target_id + len - 5, ".json") == 0) {
            target_id[len - 5] = '\0';
        }
    }

    char filepath[2048];
    snprintf(filepath, sizeof(filepath), "%s/%s.json", dir, target_id);
    puci_status st = puci_session_load_file(sess, filepath);
    if (st == PUCI_OK) {
        if (out_id && id_cap > 0) {
            snprintf(out_id, id_cap, "%s", target_id);
        }
        return 0;
    }
    return -1;
}

static void extract_json_field(const char *json, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (p) {
        p += strlen(needle);
        const char *end = strchr(p, '"');
        if (end) {
            size_t n = (size_t)(end - p);
            if (n >= cap) n = cap - 1;
            memcpy(out, p, n);
            out[n] = '\0';
        }
    }
}

int puci_cli_list_sessions(void) {
    char dir[1024];
    if (puci_cli_get_sessions_dir(dir, sizeof(dir)) != 0) {
        printf("No saved sessions found.\n");
        return 0;
    }
    DIR *d = opendir(dir);
    if (!d) {
        printf("No saved sessions found in %s\n", dir);
        return 0;
    }

    struct dirent *ent;
    int count = 0;
    printf("%-20s  %-6s  %-16s  %-15s  %s\n", "SESSION ID", "TURNS", "LAST UPDATED", "MODEL", "LAST PROMPT");
    printf("--------------------------------------------------------------------------------\n");

    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len > 5 && strcmp(ent->d_name + len - 5, ".json") == 0) {
            char full[2048];
            snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }

            char sess_id[128];
            snprintf(sess_id, sizeof(sess_id), "%.*s", (int)(len - 5), ent->d_name);

            char time_buf[64] = "";
            struct tm tm_buf;
            localtime_r(&st.st_mtime, &tm_buf);
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M", &tm_buf);

            FILE *f = fopen(full, "r");
            char model[64] = "-";
            int turns = 0;
            char last_prompt[64] = "";
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 2 * 1024 * 1024) {
                    char *buf = (char *)malloc((size_t)sz + 1);
                    if (buf) {
                        size_t rd = fread(buf, 1, (size_t)sz, f);
                        buf[rd] = '\0';
                        extract_json_field(buf, "model", model, sizeof(model));

                        /* Count user messages */
                        const char *cur = buf;
                        while ((cur = strstr(cur, "\"role\":\"user\"")) != NULL) {
                            turns++;
                            const char *cp = strstr(cur, "\"content\":\"");
                            if (cp) {
                                cp += 11;
                                const char *cend = strchr(cp, '"');
                                if (cend) {
                                    size_t n = (size_t)(cend - cp);
                                    if (n > sizeof(last_prompt) - 4) n = sizeof(last_prompt) - 4;
                                    memcpy(last_prompt, cp, n);
                                    last_prompt[n] = '\0';
                                    if ((size_t)(cend - cp) > n) {
                                        strcat(last_prompt, "...");
                                    }
                                }
                            }
                            cur += 13;
                        }
                        free(buf);
                    }
                }
                fclose(f);
            }

            printf("%-20s  %-6d  %-16s  %-15s  \"%s\"\n", sess_id, turns, time_buf, model[0] ? model : "-", last_prompt);
            count++;
        }
    }
    closedir(d);

    if (count == 0) {
        printf("No saved sessions found in %s\n", dir);
    }
    return 0;
}
