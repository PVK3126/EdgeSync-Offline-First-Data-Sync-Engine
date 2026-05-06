#include <stdio.h>
#include <stdlib.h>
#include "common.h"

// Helper to update file table status
void update_file_status(const char* target_file, int new_version, int new_status, const char* username) {
    char table_path[150];
    char tmp_path[150];
    snprintf(table_path, sizeof(table_path), "client_files_%s/file_table.txt", username);
    snprintf(tmp_path, sizeof(tmp_path), "client_files_%s/file_table_tmp.txt", username);

    FILE* f = fopen(table_path, "r");
    FILE* temp = fopen(tmp_path, "w");
    
    char line[512];
    int found = 0;
    
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char filename[MAX_FILENAME];
            int version, status;
            if (sscanf(line, "%s %d %d", filename, &version, &status) == 3) {
                if (strcmp(filename, target_file) == 0) {
                    fprintf(temp, "%s %d %d\n", filename, new_version, new_status);
                    found = 1;
                } else {
                    fprintf(temp, "%s %d %d\n", filename, version, status);
                }
            }
        }
        fclose(f);
    }
    
    if (!found) {
        fprintf(temp, "%s %d %d\n", target_file, new_version, new_status);
    }
    fclose(temp);
    
    remove(table_path);
    rename(tmp_path, table_path);
}

int get_file_info(const char* target_file, int* version, int* status, const char* username) {
    char table_path[150];
    snprintf(table_path, sizeof(table_path), "client_files_%s/file_table.txt", username);
    FILE* f = fopen(table_path, "r");
    if (!f) return 0;
    
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char filename[MAX_FILENAME];
        int v, s;
        if (sscanf(line, "%s %d %d", filename, &v, &s) == 3) {
            if (strcmp(filename, target_file) == 0) {
                *version = v;
                *status = s;
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

void sync_file_with_server(const char* filename, Role user_role, const char* username) {
    int version = 0;
    int status = STATUS_SYNCED;
    if (!get_file_info(filename, &version, &status, username)) {
        return;
    }

    if (status != STATUS_UNSYNCED && status != STATUS_CONFLICT) {
        return; // Nothing to sync
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        // Server not available, will retry later
        close(sock);
        return;
    }

    char filepath[MAX_FILENAME + 100];
    snprintf(filepath, sizeof(filepath), "client_files_%s/%s", username, filename);
    
    FILE* f = fopen(filepath, "rb");
    int content_length = 0;
    char* content = NULL;
    
    if (f) {
        fseek(f, 0, SEEK_END);
        content_length = ftell(f);
        fseek(f, 0, SEEK_SET);
        content = malloc(content_length);
        fread(content, 1, content_length, f);
        fclose(f);
    }

    PacketHeader header;
    memset(&header, 0, sizeof(header));
    header.cmd = CMD_SYNC;
    strcpy(header.filename, filename);
    header.version = version;
    header.role = user_role;
    header.content_length = content_length;

    send(sock, &header, sizeof(header), 0);
    if (content_length > 0) {
        send(sock, content, content_length, 0);
        free(content);
    }

    ResponseHeader resp;
    if (recv(sock, &resp, sizeof(resp), 0) > 0) {
        if (resp.status == 0) { // OK
            printf("[DAEMON] Sync successful for %s (user: %s). Version %d accepted.\n",
                   filename, username, resp.version);
            update_file_status(filename, version, STATUS_SYNCED, username);
        } else if (resp.status == 1) { // CONFLICT
            printf("[DAEMON] CONFLICT detected for %s (user: %s)! Server is at version %d.\n",
                   filename, username, resp.version);
            update_file_status(filename, version, STATUS_CONFLICT, username);
            
            // Save server's version as .server file
            if (resp.content_length > 0) {
                char* s_content = malloc(resp.content_length + 1);
                int received = 0;
                while (received < resp.content_length) {
                    int bytes = recv(sock, s_content + received, resp.content_length - received, 0);
                    if (bytes <= 0) break;
                    received += bytes;
                }
                char conflict_filepath[MAX_FILENAME + 100];
                snprintf(conflict_filepath, sizeof(conflict_filepath),
                         "client_files_%s/%s.server", username, filename);
                FILE* cf = fopen(conflict_filepath, "wb");
                if (cf) {
                    fwrite(s_content, 1, resp.content_length, cf);
                    fclose(cf);
                }
                free(s_content);
            }
        } else if (resp.status == 2) {
            printf("[DAEMON] Access Denied by Server for %s.\n", filename);
        }
    }

    close(sock);
}

// ─────────────────────────────────────────────────────────────────
// AUTO-RETRY THREAD: Scans all client_files_* dirs every 5 seconds
// and retries any STATUS_UNSYNCED files automatically.
// ─────────────────────────────────────────────────────────────────
void* auto_retry_thread(void* arg) {
    (void)arg;
    while (1) {
        sleep(5); // Wait 5 seconds between scans

        DIR* d = opendir(".");
        if (!d) continue;

        struct dirent* dir;
        int found_any = 0;

        while ((dir = readdir(d)) != NULL) {
            if (strncmp(dir->d_name, "client_files_", 13) != 0) continue;

            char* username = dir->d_name + 13;
            char table_path[200];
            snprintf(table_path, sizeof(table_path), "%s/file_table.txt", dir->d_name);

            FILE* ft = fopen(table_path, "r");
            if (!ft) continue;

            char line[512];
            while (fgets(line, sizeof(line), ft)) {
                char filename[MAX_FILENAME];
                int version, status;
                if (sscanf(line, "%s %d %d", filename, &version, &status) == 3) {
                    if (status == STATUS_UNSYNCED) {
                        if (!found_any) {
                            printf("[DAEMON] Auto-retry: Found unsynced files. Attempting sync...\n");
                            found_any = 1;
                        }
                        printf("[DAEMON] Auto-retry: Syncing %s for user %s...\n", filename, username);
                        sync_file_with_server(filename, ROLE_USER, username);
                    }
                }
            }
            fclose(ft);
        }
        closedir(d);
    }
    return NULL;
}

int main() {
    // Create the IPC Pipe
    unlink(IPC_PIPE_NAME);
    if (mkfifo(IPC_PIPE_NAME, 0666) == -1) {
        perror("mkfifo failed");
        exit(1);
    }

    printf("[DAEMON] Sync Daemon started. Listening for IPC messages on %s...\n", IPC_PIPE_NAME);
    printf("[DAEMON] Auto-retry enabled: will re-attempt unsynced files every 5 seconds.\n");

    // Start the background auto-retry thread
    pthread_t retry_thread;
    pthread_create(&retry_thread, NULL, auto_retry_thread, NULL);
    pthread_detach(retry_thread);

    // Main loop: listen for IPC messages from client_app
    char buffer[MAX_BUFFER];
    while (1) {
        int pipe_fd = open(IPC_PIPE_NAME, O_RDONLY);
        if (pipe_fd == -1) {
            perror("open pipe failed");
            continue;
        }

        int bytes_read = read(pipe_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            printf("[DAEMON] IPC Received: %s\n", buffer);
            
            // Expected format: "SYNC filename role_id username"
            char cmd[32];
            char filename[MAX_FILENAME];
            int role_id = 0;
            char username[50] = "";
            
            if (sscanf(buffer, "%s %s %d %s", cmd, filename, &role_id, username) >= 4) {
                if (strcmp(cmd, "SYNC") == 0) {
                    sync_file_with_server(filename, (Role)role_id, username);
                }
            }
        }
        close(pipe_fd);
    }

    unlink(IPC_PIPE_NAME);
    return 0;
}
