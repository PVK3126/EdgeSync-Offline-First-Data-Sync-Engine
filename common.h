#ifndef COMMON_H
#define COMMON_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#define SERVER_PORT 8080
#define MAX_BUFFER 4096
#define MAX_FILENAME 256
#define IPC_PIPE_NAME "/tmp/edgesync_ipc_pipe"

// Roles for authorization
typedef enum { ROLE_GUEST = 0, ROLE_USER = 1, ROLE_ADMIN = 2 } Role;

// Commands sent from client to server
typedef enum {
  CMD_READ = 1,
  CMD_SYNC = 2,
  CMD_RESOLVE_CONFLICT = 3
} CommandType;

// Network protocol structures
typedef struct {
  CommandType cmd;
  char filename[MAX_FILENAME];
  int version;
  Role role;
  int content_length;
  // Followed by raw content in socket
} PacketHeader;

typedef struct {
  int status; // 0=OK, 1=CONFLICT, 2=ACCESS_DENIED, 3=NOT_FOUND, 4=ERROR
  int version;
  int content_length;
  // Followed by raw content in socket
} ResponseHeader;

// File statuses on client
#define STATUS_SYNCED 0
#define STATUS_UNSYNCED 1
#define STATUS_CONFLICT 2

#endif
