#include "common.h"
#include <ctype.h>

int is_valid_username(const char *username) {
  for (int i = 0; username[i] != '\0'; i++) {
    if (isalpha((unsigned char)username[i])) {
      return 1;
    }
  }
  return 0;
}

Role current_role = ROLE_GUEST;
char current_username[50] = "";

void update_local_file_table(const char *filename, int *new_version_out) {
  char table_path[150];
  char tmp_path[150];
  snprintf(table_path, sizeof(table_path), "client_files_%s/file_table.txt",
           current_username);
  snprintf(tmp_path, sizeof(tmp_path), "client_files_%s/file_table_tmp.txt",
           current_username);

  FILE *f = fopen(table_path, "r");
  FILE *temp = fopen(tmp_path, "w");

  char line[512];
  int found = 0;
  int new_version = 1;

  if (f) {
    while (fgets(line, sizeof(line), f)) {
      char fname[MAX_FILENAME];
      int version, status;
      if (sscanf(line, "%s %d %d", fname, &version, &status) == 3) {
        if (strcmp(fname, filename) == 0) {
          new_version = version + 1; // Phase 4: Versioning
          fprintf(temp, "%s %d %d\n", fname, new_version, STATUS_UNSYNCED);
          found = 1;
        } else {
          fprintf(temp, "%s %d %d\n", fname, version, status);
        }
      }
    }
    fclose(f);
  }

  if (!found) {
    fprintf(temp, "%s %d %d\n", filename, new_version, STATUS_UNSYNCED);
  }
  fclose(temp);

  remove(table_path);
  rename(tmp_path, table_path);

  if (new_version_out)
    *new_version_out = new_version;
}

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
      char fname[MAX_FILENAME];
      int v, status;
      if (sscanf(line, "%s %d %d", fname, &v, &status) == 3) {
        if (strcmp(fname, target_file) == 0) {
          fprintf(temp, "%s %d %d\n", fname, new_version, new_status);
          found = 1;
        } else {
          fprintf(temp, "%s %d %d\n", fname, v, status);
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

void trigger_sync(const char *filename) {
  // Send IPC message to sync daemon (Phase 9)
  int pipe_fd = open(IPC_PIPE_NAME, O_WRONLY);
  if (pipe_fd == -1) {
    printf("[ERROR] Sync daemon not running. Changes saved offline.\n");
    return;
  }

  char msg[MAX_BUFFER];
  snprintf(msg, sizeof(msg), "SYNC %s %d %s", filename, current_role,
           current_username);
  write(pipe_fd, msg, strlen(msg));
  close(pipe_fd);
  printf("[CLIENT] Triggered background sync for %s.\n", filename);
}

void edit_file() {
  if (current_role == ROLE_GUEST) {
    printf("[ERROR] Guests cannot edit files.\n");
    return;
  }

  char filename[MAX_FILENAME];
  printf("Enter filename to edit: ");
  scanf("%s", filename);

  char content[MAX_BUFFER];
  printf("Enter content (single line): ");
  getchar(); // consume newline left by scanf
  fgets(content, sizeof(content), stdin);
  content[strcspn(content, "\n")] = 0; // remove trailing newline
  char filepath[MAX_FILENAME + 100];
  snprintf(filepath, sizeof(filepath), "client_files_%s/%s", current_username,
           filename);

  FILE *f = fopen(filepath, "w");
  if (!f) {
    printf("[ERROR] Cannot open file.\n");
    return;
  }
  fprintf(f, "%s", content);
  fclose(f);

  int new_version;
  update_local_file_table(filename, &new_version);
  printf("File saved offline. Version bumped to %d.\n", new_version);

  trigger_sync(filename);
}

void read_file() {
  char filename[MAX_FILENAME];
  printf("Enter filename to read: ");
  scanf("%s", filename);

  char filepath[MAX_FILENAME + 100];
  snprintf(filepath, sizeof(filepath), "client_files_%s/%s", current_username,
           filename);

  FILE *f = fopen(filepath, "r");
  if (f) {
    char content[MAX_BUFFER];
    if (fgets(content, sizeof(content), f)) {
      printf("Content: %s\n", content);
    } else {
      printf("File is empty.\n");
    }
    fclose(f);
  } else {
    // If not found locally, try to pull from server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == 0) {
      PacketHeader header = {0};
      header.cmd = CMD_READ;
      strcpy(header.filename, filename);
      header.role = current_role;
      header.content_length = 0;

      send(sock, &header, sizeof(header), 0);

      ResponseHeader resp;
      if (recv(sock, &resp, sizeof(resp), 0) > 0) {
        if (resp.status == 0) {
          char *s_content = malloc(resp.content_length + 1);
          int received = 0;
          while (received < resp.content_length) {
              int bytes = recv(sock, s_content + received, resp.content_length - received, 0);
              if (bytes <= 0) break;
              received += bytes;
          }
          s_content[resp.content_length] = '\0';
          
          printf("Content (fetched from server): %s\n", s_content);
          
          // Save it locally for offline access later
          FILE *wf = fopen(filepath, "w");
          if (wf) {
              fprintf(wf, "%s", s_content);
              fclose(wf);
          }
          
          // Update file table so daemon knows we have it
          update_file_status(filename, resp.version, STATUS_SYNCED, current_username);
          
          free(s_content);
        } else if (resp.status == 3) {
          printf("[ERROR] File not found on server either.\n");
        } else if (resp.status == 2) {
          printf("[ERROR] Access Denied by Server.\n");
        } else {
          printf("[ERROR] Failed to fetch from server.\n");
        }
      }
    } else {
      printf("[ERROR] Local file not found and cannot connect to server.\n");
    }
    close(sock);
  }
}

void resolve_conflict() {
  if (current_role != ROLE_ADMIN) {
    printf("[ERROR] Only ADMIN can resolve conflicts.\n");
    return;
  }

  char filename[MAX_FILENAME];
  printf("Enter filename to resolve: ");
  scanf("%s", filename);

  char target_users[10][50];
  char filepaths[10][MAX_FILENAME + 100];
  char s_filepaths[10][MAX_FILENAME + 100];
  int num_conflicts = 0;
  int version = 0;

  DIR *d = opendir(".");
  if (d) {
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
      if (strncmp(dir->d_name, "client_files_", 13) == 0) {
        char *uname = dir->d_name + 13;
        char temp_s_filepath[MAX_FILENAME + 150];
        snprintf(temp_s_filepath, sizeof(temp_s_filepath), "%s/%s.server", dir->d_name, filename);
        
        FILE *tf = fopen(temp_s_filepath, "r");
        if (tf) {
          fclose(tf);
          strcpy(target_users[num_conflicts], uname);
          snprintf(filepaths[num_conflicts], sizeof(filepaths[0]), "%s/%s", dir->d_name, filename);
          strcpy(s_filepaths[num_conflicts], temp_s_filepath);
          
          if (num_conflicts == 0) {
            // Read version from the first user's file_table
            char table_path[MAX_FILENAME + 150];
            snprintf(table_path, sizeof(table_path), "%s/file_table.txt", dir->d_name);
            FILE *f = fopen(table_path, "r");
            if (f) {
              char line[512];
              while (fgets(line, sizeof(line), f)) {
                char fname[MAX_FILENAME];
                int v, s;
                if (sscanf(line, "%s %d %d", fname, &v, &s) == 3) {
                  if (strcmp(fname, filename) == 0) {
                    version = v;
                    break;
                  }
                }
              }
              fclose(f);
            }
          }
          num_conflicts++;
          if (num_conflicts >= 10) break; // Arbitrary limit for simplicity
        }
      }
    }
    closedir(d);
  }

  if (num_conflicts == 0) {
    printf("File is not in conflict state.\n");
    return;
  }

  printf("\n--- CONFLICT DETECTED FOR %s (%d Users) ---\n", filename, num_conflicts);

  // Print Local Versions
  for (int i = 0; i < num_conflicts; i++) {
    printf(">> Local Version (User: %s):\n", target_users[i]);
    FILE *lf_read = fopen(filepaths[i], "r");
    if (lf_read) {
      char content[MAX_BUFFER];
      if (fgets(content, sizeof(content), lf_read))
        printf("%s\n", content);
      fclose(lf_read);
    }
  }

  // Print Server Version
  printf("\n>> Server Version (Theirs):\n");
  FILE *sf_read = fopen(s_filepaths[0], "r");
  if (sf_read) {
    char content[MAX_BUFFER];
    if (fgets(content, sizeof(content), sf_read))
      printf("%s\n", content);
    fclose(sf_read);
  }

  printf("\n--- RESOLUTION ---\n");
  printf("1. Auto-Merge ALL (Combine all versions with conflict markers)\n");
  printf("2. Keep Server Version (Discard all local changes)\n");
  printf("Choice: ");
  int choice;
  scanf("%d", &choice);

  if (choice == 1) {
    // Auto-build merged content with git-style conflict markers
    char merged_content[MAX_BUFFER];
    merged_content[0] = '\0';

    // Read server version
    char server_content[MAX_BUFFER] = "[empty]";
    FILE *sv = fopen(s_filepaths[0], "r");
    if (sv) {
      if (fgets(server_content, sizeof(server_content), sv))
        server_content[strcspn(server_content, "\n")] = 0;
      fclose(sv);
    }

    // Build the merged string: for each user, add their section
    for (int i = 0; i < num_conflicts; i++) {
      char user_content[MAX_BUFFER] = "[empty]";
      FILE *uf = fopen(filepaths[i], "r");
      if (uf) {
        if (fgets(user_content, sizeof(user_content), uf))
          user_content[strcspn(user_content, "\n")] = 0;
        fclose(uf);
      }

      char section[MAX_BUFFER];
      snprintf(section, sizeof(section),
        "<<<< %s\n%s\n====\n%s\n>>>> SERVER\n",
        target_users[i], user_content, server_content);

      strncat(merged_content, section, sizeof(merged_content) - strlen(merged_content) - 1);
    }

    printf("\n[AUTO-MERGED CONTENT]:\n%s\n", merged_content);

    // Send merged version to server with CMD_RESOLVE_CONFLICT
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == 0) {
      int len = strlen(merged_content);

      PacketHeader header = {0};
      header.cmd = CMD_RESOLVE_CONFLICT;
      strcpy(header.filename, filename);
      header.version = version;
      header.role = current_role;
      header.content_length = len;

      send(sock, &header, sizeof(header), 0);
      send(sock, merged_content, len, 0);

      ResponseHeader resp;
      if (recv(sock, &resp, sizeof(resp), 0) > 0 && resp.status == 0) {
        printf("Resolved! Auto-merged version pushed to server.\n");
        // Update local files for ALL conflicted users
        for (int i = 0; i < num_conflicts; i++) {
          FILE *wlf = fopen(filepaths[i], "w");
          if (wlf) {
            fprintf(wlf, "%s", merged_content);
            fclose(wlf);
          }
          update_file_status(filename, version, STATUS_SYNCED, target_users[i]);
          remove(s_filepaths[i]);
        }
      } else {
        printf("[ERROR] Server rejected resolution.\n");
      }
    }
    close(sock);
  } else if (choice == 2) {
    printf("Resolved! Server version kept locally for all users.\n");
    for (int i = 0; i < num_conflicts; i++) {
      remove(filepaths[i]);
      rename(s_filepaths[i], filepaths[i]);
      update_file_status(filename, version, STATUS_SYNCED, target_users[i]);
    }
  } else {
    printf("Resolution cancelled.\n");
  }
}

int main() {
  printf("Welcome to EdgeSync Client\n");
  char username[50];
  char password[50];

  while (1) {
    printf("\n--- Welcome to EdgeSync Client ---\n");
    printf("1. Create User\n");
    printf("2. User Login\n");
    printf("3. Admin Login\n");
    printf("4. Guest Login\n");
    printf("5. Exit\n");
    printf("Choice: ");
    
    int choice;
    if (scanf("%d", &choice) != 1) {
      while (getchar() != '\n'); // clear invalid input
      printf("Invalid input.\n");
      continue;
    }

    if (choice == 5) {
      return 0;
    }

    if (choice == 3) {
      printf("Admin Password (or type 'back'): ");
      scanf("%s", password);
      if (strcmp(password, "back") == 0) continue;

      if (strcmp(password, "admin123") == 0) {
        current_role = ROLE_ADMIN;
        strcpy(current_username, "admin");
        printf("Logged in as ADMIN.\n");
        break;
      } else {
        printf("Invalid Admin password. Try again.\n");
      }
    } else if (choice == 1) {
      // Create User
      printf("Choose Role for new user (1=GUEST, 2=USER, or 0=back): ");
      int role_choice;
      if (scanf("%d", &role_choice) != 1) {
        while(getchar() != '\n');
        printf("Invalid input.\n");
        continue;
      }
      
      if (role_choice == 0) continue;
      if (role_choice != 1 && role_choice != 2) {
        printf("Invalid role selection.\n");
        continue;
      }

      printf("Enter new Username (or type 'back'): ");
      scanf("%s", username);
      if (strcmp(username, "back") == 0) continue;

      if (!is_valid_username(username)) {
        printf("Invalid username! Must contain at least one alphabet character.\n");
        continue;
      }

      if (strcasecmp(username, "admin") == 0) {
        printf("Invalid username! 'admin' is a reserved name.\n");
        continue;
      }

      // Check if user already exists
      FILE *uf = fopen("users.txt", "r");
      int found = 0;
      if (uf) {
        char f_user[50], f_pass[50];
        int f_role;
        while (fscanf(uf, "%s %s %d", f_user, f_pass, &f_role) == 3) {
          if (strcmp(f_user, username) == 0) {
            found = 1;
            break;
          }
        }
        fclose(uf);
      }

      if (found) {
        printf("Error: Username already exists! Try logging in instead.\n");
        continue;
      }

      printf("Enter new Password: ");
      scanf("%s", password);

      uf = fopen("users.txt", "a");
      if (uf) {
        fprintf(uf, "%s %s %d\n", username, password, role_choice);
        fclose(uf);
        printf("New user registered successfully! You can now log in from the main menu.\n");
      } else {
        printf("Error creating user file.\n");
      }

    } else if (choice == 2 || choice == 4) {
      // User or Guest Login
      printf("Username (or type 'back'): ");
      scanf("%s", username);
      if (strcmp(username, "back") == 0) continue;

      printf("Password: ");
      scanf("%s", password);

      FILE *uf = fopen("users.txt", "r");
      int found = 0;
      int pass_match = 0;
      // In users.txt, 1 = GUEST, 2 = USER
      int expected_file_role = (choice == 4) ? 1 : 2;
      int actual_file_role = -1;

      if (uf) {
        char f_user[50], f_pass[50];
        int f_role;
        while (fscanf(uf, "%s %s %d", f_user, f_pass, &f_role) == 3) {
          if (strcmp(f_user, username) == 0) {
            found = 1;
            if (strcmp(f_pass, password) == 0) {
              pass_match = 1;
              actual_file_role = f_role;
            }
            break;
          }
        }
        fclose(uf);
      }

      if (found && pass_match) {
        if (actual_file_role != expected_file_role) {
          printf("Error: This account is registered as a different role (Expected: %s, Found: %s).\n", 
            (expected_file_role == 1) ? "GUEST" : "USER",
            (actual_file_role == 1) ? "GUEST" : "USER");
        } else {
          current_role = (actual_file_role == 1) ? ROLE_GUEST : ROLE_USER;
          strcpy(current_username, username);
          printf("Logged in successfully as %s.\n", username);
          break;
        }
      } else {
        printf("Invalid credentials or user does not exist.\n");
      }
    } else {
      printf("Invalid choice. Please select 1-5.\n");
    }
  }

  char dir_name[100];
  snprintf(dir_name, sizeof(dir_name), "client_files_%s", current_username);
  mkdir(dir_name, 0777);

  while (1) {
    printf("\n--- EdgeSync Menu ---\n");
    printf("1. Read File\n");
    printf("2. Edit/Create File\n");
    printf("3. Resolve Conflict (Admin Only)\n");
    printf("4. Exit\n");
    printf("Choice: ");
    int choice;
    scanf("%d", &choice);

    if (choice == 1)
      read_file();
    else if (choice == 2)
      edit_file();
    else if (choice == 3)
      resolve_conflict();
    else if (choice == 4)
      break;
  }

  return 0;
}
