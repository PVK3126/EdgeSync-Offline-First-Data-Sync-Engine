#include "common.h"

#define MAX_FILES 100

typedef struct {
  char filename[MAX_FILENAME];
  int version;
  pthread_mutex_t lock;
} ServerFile;

ServerFile server_files[MAX_FILES];
int num_files = 0;
pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

// ── Persist version numbers so server restart doesn't lose them ──
void save_versions() {
  FILE *f = fopen("server_files/versions.txt", "w");
  if (!f) return;
  for (int i = 0; i < num_files; i++) {
    fprintf(f, "%s %d\n", server_files[i].filename, server_files[i].version);
  }
  fclose(f);
}

void load_versions() {
  FILE *f = fopen("server_files/versions.txt", "r");
  if (!f) return;
  char fname[MAX_FILENAME];
  int ver;
  while (fscanf(f, "%s %d", fname, &ver) == 2) {
    if (num_files < MAX_FILES) {
      strcpy(server_files[num_files].filename, fname);
      server_files[num_files].version = ver;
      pthread_mutex_init(&server_files[num_files].lock, NULL);
      printf("[SERVER] Loaded persisted version: %s = v%d\n", fname, ver);
      num_files++;
    }
  }
  fclose(f);
}

ServerFile *get_or_create_file(const char *filename) {
  pthread_mutex_lock(&global_lock);
  for (int i = 0; i < num_files; i++) {
    if (strcmp(server_files[i].filename, filename) == 0) {
      pthread_mutex_unlock(&global_lock);
      return &server_files[i];
    }
  }
  if (num_files < MAX_FILES) {
    strcpy(server_files[num_files].filename, filename);
    server_files[num_files].version = 0;
    pthread_mutex_init(&server_files[num_files].lock, NULL);
    num_files++;
    pthread_mutex_unlock(&global_lock);
    return &server_files[num_files - 1];
  }
  pthread_mutex_unlock(&global_lock);
  return NULL;
}

void *handle_client(void *client_socket_ptr) {
  int client_socket = *(int *)client_socket_ptr;
  free(client_socket_ptr);

  PacketHeader header;
  if (recv(client_socket, &header, sizeof(PacketHeader), 0) <= 0) {
    close(client_socket);
    return NULL;
  }

  char *content = NULL;
  if (header.content_length > 0) {
    content = malloc(header.content_length + 1);
    int received = 0;
    while (received < header.content_length) {
      int bytes = recv(client_socket, content + received,
                       header.content_length - received, 0);
      if (bytes <= 0)
        break;
      received += bytes;
    }
    content[header.content_length] = '\0';
  }

  ResponseHeader resp = {0};
  ServerFile *sf = get_or_create_file(header.filename);

  if (!sf) {
    resp.status = 4; // ERROR
    send(client_socket, &resp, sizeof(ResponseHeader), 0);
    if (content)
      free(content);
    close(client_socket);
    return NULL;
  }

  printf("[SERVER] Received CMD=%d, File=%s, Ver=%d, Role=%d\n", header.cmd,
         header.filename, header.version, header.role);

  // LOCK THE FILE: Ensure concurrency safety (Phase 6)
  pthread_mutex_lock(&sf->lock);

  if (header.cmd == CMD_SYNC) {
    if (header.role == ROLE_GUEST) {
      resp.status = 2; // ACCESS DENIED
    } else {
      // CONFLICT DETECTION (Phase 7)
      // Expecting client version to be exactly server version + 1 or 1 if new
      if (sf->version > 0 && header.version != sf->version + 1) {
        printf("[SERVER] CONFLICT DETECTED on %s! Server Ver: %d, Client Ver: "
               "%d\n",
               header.filename, sf->version, header.version);
        resp.status = 1; // CONFLICT
        resp.version = sf->version;

        // Send current server file back to client
        char filepath[MAX_FILENAME + 32];
        snprintf(filepath, sizeof(filepath), "server_files/%s",
                 header.filename);
        FILE *f = fopen(filepath, "rb");
        if (f) {
          fseek(f, 0, SEEK_END);
          resp.content_length = ftell(f);
          fseek(f, 0, SEEK_SET);
          char *s_content = malloc(resp.content_length);
          fread(s_content, 1, resp.content_length, f);
          fclose(f);
          send(client_socket, &resp, sizeof(ResponseHeader), 0);
          send(client_socket, s_content, resp.content_length, 0);
          free(s_content);
        } else {
          resp.content_length = 0;
          send(client_socket, &resp, sizeof(ResponseHeader), 0);
        }
      } else {
        // Accept sync
        char filepath[MAX_FILENAME + 32];
        snprintf(filepath, sizeof(filepath), "server_files/%s",
                 header.filename);
        FILE *f = fopen(filepath, "wb");
        if (f && content) {
          fwrite(content, 1, header.content_length, f);
          fclose(f);
        }
        sf->version = header.version;
        resp.status = 0; // OK
        resp.version = sf->version;
        resp.content_length = 0;
        send(client_socket, &resp, sizeof(ResponseHeader), 0);
        printf("[SERVER] Synced %s to version %d\n", header.filename, sf->version);
        save_versions(); // Persist version to disk
      }
    }
  } else if (header.cmd == CMD_RESOLVE_CONFLICT) {
    // ROLE-BASED AUTHORIZATION (Phase 10)
    if (header.role != ROLE_ADMIN) {
      printf("[SERVER] Access Denied: User attempted to resolve conflict "
             "without Admin role.\n");
      resp.status = 2; // ACCESS DENIED
      resp.content_length = 0;
      send(client_socket, &resp, sizeof(ResponseHeader), 0);
    } else {
      // Admin overwrites the file
      char filepath[MAX_FILENAME + 32];
      snprintf(filepath, sizeof(filepath), "server_files/%s", header.filename);
      FILE *f = fopen(filepath, "wb");
      if (f && content) {
        fwrite(content, 1, header.content_length, f);
        fclose(f);
      }
      // If admin resolves, their version becomes the new source of truth
      sf->version = header.version;
      resp.status = 0; // OK
      resp.version = sf->version;
      resp.content_length = 0;
      send(client_socket, &resp, sizeof(ResponseHeader), 0);
      printf("[SERVER] Admin resolved conflict for %s. New version: %d\n",
             header.filename, sf->version);
      save_versions(); // Persist version to disk
    }
  } else if (header.cmd == CMD_READ) {
    char filepath[MAX_FILENAME + 32];
    snprintf(filepath, sizeof(filepath), "server_files/%s", header.filename);
    FILE *f = fopen(filepath, "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      resp.content_length = ftell(f);
      fseek(f, 0, SEEK_SET);
      char *s_content = malloc(resp.content_length);
      fread(s_content, 1, resp.content_length, f);
      fclose(f);

      resp.status = 0;
      resp.version = sf->version;
      send(client_socket, &resp, sizeof(ResponseHeader), 0);
      send(client_socket, s_content, resp.content_length, 0);
      free(s_content);
    } else {
      resp.status = 3; // NOT FOUND
      resp.content_length = 0;
      send(client_socket, &resp, sizeof(ResponseHeader), 0);
    }
  }

  // UNLOCK THE FILE
  pthread_mutex_unlock(&sf->lock);

  if (content)
    free(content);
  close(client_socket);
  return NULL;
}

int main() {
  // Create server directory
  mkdir("server_files", 0777);

  int server_socket;
  struct sockaddr_in server_addr, client_addr;
  socklen_t client_len = sizeof(client_addr);

  server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket < 0) {
    perror("Socket creation failed");
    exit(1);
  }

  int opt = 1;
  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(SERVER_PORT);

  if (bind(server_socket, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    perror("Bind failed");
    exit(1);
  }

  if (listen(server_socket, 10) < 0) {
    perror("Listen failed");
    exit(1);
  }

  printf("EdgeSync Server listening on port %d...\n", SERVER_PORT);
  load_versions(); // Restore persisted version numbers after restart

  while (1) {
    int *client_socket = malloc(sizeof(int));
    *client_socket =
        accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
    if (*client_socket < 0) {
      perror("Accept failed");
      free(client_socket);
      continue;
    }

    // CONCURRENCY: Create a new thread for each client (Phase 2)
    pthread_t thread;
    pthread_create(&thread, NULL, handle_client, client_socket);
    pthread_detach(thread);
  }

  close(server_socket);
  return 0;
}
