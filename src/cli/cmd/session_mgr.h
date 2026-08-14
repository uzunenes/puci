#ifndef PUCI_CLI_SESSION_MGR_H
#define PUCI_CLI_SESSION_MGR_H

#include <puci/puci.h>
#include <stddef.h>

int puci_cli_get_sessions_dir(char *out_dir, size_t cap);
int puci_cli_get_history_file(char *out_path, size_t cap);

int puci_cli_save_session(const puci_session *sess, const char *session_id);
int puci_cli_load_session(puci_session *sess, const char *id_or_latest, char *out_id, size_t id_cap);
int puci_cli_list_sessions(void);

#endif /* PUCI_CLI_SESSION_MGR_H */
