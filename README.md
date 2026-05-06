# EdgeSync: Offline-First Data Synchronization Engine

EdgeSync is a professional-grade synchronization engine built in C, designed to handle multi-user collaboration in environments with intermittent connectivity. It follows an **Offline-First** architecture, allowing users to continue working without an internet connection and automatically synchronizing changes when the server becomes available.

##  Key Features

- **Offline-First Logic**: Edits are stored locally when the server is unreachable.
- **Background Sync Daemon**: A background process that uses IPC (Named Pipes) to listen for local changes and automatically retries synchronization every 5 seconds.
- **Role-Based Access Control**:
  - **Admin**: Resolves conflicts and manages the global state.
  - **User**: Can read and edit files.
  - **Guest**: Read-only access.
- **Smart Conflict Resolution**: When multiple users edit the same file offline, the Admin can perform an **Auto-Merge** which generates Git-style conflict markers (`<<<<`, `====`, `>>>>`).
- **Persistence**: Server remembers file versions across restarts using a version tracking system.
- **Concurrent Server**: Multi-threaded socket server capable of handling multiple simultaneous client connections.

##  Architecture

- **`server.c`**: The central authority that manages the "Source of Truth," tracks versions, and detects conflicts.
- **`sync_daemon.c`**: The background worker that bridges the gap between the local filesystem and the remote server.
- **`client_app.c`**: The user interface (CLI) for logging in, editing files, and resolving conflicts.
- **`common.h`**: Shared protocols, packet headers, and status constants.

## Setup & Installation

1. **Prerequisites**: Ensure you are on a Linux/Unix environment (WSL Ubuntu is recommended) with `gcc` and `make` installed.
2. **Clone & Build**:
   ```bash
   cd EdgeSync-Offline-First-Data-Sync-Engine
   make -B
   ```

## Running the Demo (The "Online-Offline-Online" Lifecycle)

To see the engine in action, follow these steps across multiple terminals:

### 1. Start the Infrastructure
- **Terminal 1**: Run the server.
  ```bash
  ./server
  ```
- **Terminal 2**: Run the background daemon.
  ```bash
  ./sync_daemon
  ```

### 2. Set the Baseline (Online)
- **Terminal 3**: Log in as `krishna` and edit `d.txt`.
  ```bash
  ./client_app
  # Select Login -> User: krishna -> Edit d.txt -> "krishna_baseline"
  ```
- The daemon will show: `Sync successful... Version 1 accepted.`

### 3. Simulate Offline State
- **Terminal 1**: Stop the server (`Ctrl+C`).

### 4. Concurrent Offline Edits
- **Terminal 3**: Log in as `pramod` and edit `d.txt` offline.
  ```bash
  # Edit d.txt -> "pramod_offline"
  ```
- **Terminal 4**: Log in as `moksha` and edit `d.txt` offline.
  ```bash
  # Edit d.txt -> "moksha_offline"
  ```
- The daemon will begin retrying these files every 5 seconds.

### 5. Restore Connectivity & Conflict Resolution
- **Terminal 1**: Restart the server (`./server`).
- The daemon will automatically detect the server and attempt to sync. Both users will move to a `CONFLICT` state because they are both trying to update Version 1.
- **Terminal 5 (Admin)**: Log in as `admin`.
  ```bash
  ./client_app
  # Select Admin Login -> Resolve Conflict -> d.txt -> Auto-Merge
  ```
- The final `d.txt` will now contain the merged contents of all users with clear conflict markers.

## Project Structure

- `client_files_[user]/`: Isolated local storage for each user.
- `server_files/`: The server's storage and version history.
- `file_table.txt`: Tracks local file status (Synced, Unsynced, Conflict).
- `versions.txt`: Persistent version tracking for the server.

