# Per-Link Relay Telemetry Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fork srtla_rec to expose per-link connection stats via HTTP, consume in C++ plugin, and display per-link throughput bars in the OBS dock UI.

**Architecture:** srtla_rec fork adds byte/packet counters per connection in the UDP packet handler and exposes them via a lightweight HTTP endpoint on port 5080. The C++ OBS plugin polls this endpoint alongside the existing SLS stats poll. Dock UI renders per-link throughput bars with traffic share percentages and a connection count badge.

**Tech Stack:** C (srtla_rec fork), Docker (custom image), Go (relay provisioning), C++ (OBS plugin), JavaScript/React (dock UI)

---

## Lane 1: srtla_rec Fork (C)

### Task 1: Fork and clone srtla repo

**Manual step** — must be done interactively on GitHub.

**Step 1: Fork on GitHub**

Go to https://github.com/OpenIRL/srtla and click "Fork" to create `telemyapp/srtla`.

**Step 2: Clone locally**

```bash
cd /e/Code/telemyapp
git clone https://github.com/telemyapp/srtla.git srtla-fork
cd srtla-fork
```

**Step 3: Verify baseline builds**

```bash
git submodule update --init
mkdir build && cd build
cmake .. && make
```

Expected: `srtla_rec` binary in build directory. (Requires Linux or WSL — this is a Linux C project.)

---

### Task 2: Add per-link counter fields to data structures

**Files:**
- Modify: `srtla_rec.c:41-56` (conn_t and conn_group_t structs)

**Step 1: Add counter fields to conn_t**

In `srtla_rec.c`, after the existing `recv_log` field in the `srtla_conn` struct (around line 46), add:

```c
typedef struct srtla_conn {
    struct srtla_conn *next;
    struct sockaddr addr;
    time_t last_rcvd;
    int recv_idx;
    uint32_t recv_log[RECV_ACK_INT];
    // Per-link telemetry counters
    uint64_t bytes_recv;
    uint64_t pkts_recv;
    struct timespec first_seen;
    struct timespec last_recv_ts;
} conn_t;
```

**Step 2: Add total counter to conn_group_t**

In the `srtla_conn_group` struct (around line 55), add:

```c
typedef struct srtla_conn_group {
    struct srtla_conn_group *next;
    conn_t *conns;
    time_t created_at;
    int srt_sock;
    struct sockaddr last_addr;
    char id[SRTLA_ID_LEN];
    // Aggregate counter for share % calculation
    uint64_t total_bytes_recv;
} conn_group_t;
```

**Step 3: Initialize counters in conn_reg()**

In the `conn_reg()` function (around line 282), where a new `conn_t` is allocated, add initialization after the existing `c->last_rcvd = ts;`:

```c
c->bytes_recv = 0;
c->pkts_recv = 0;
clock_gettime(CLOCK_MONOTONIC, &c->first_seen);
c->last_recv_ts = c->first_seen;
```

**Step 4: Verify compilation**

```bash
cd build && make
```

Expected: compiles clean, no warnings on new fields.

**Step 5: Commit**

```bash
git add srtla_rec.c
git commit -m "feat: add per-link telemetry counter fields to conn_t and conn_group_t"
```

---

### Task 3: Instrument the packet handler

**Files:**
- Modify: `srtla_rec.c` (handle_srtla_data function, around line 403)

**Step 1: Add counter increments in handle_srtla_data()**

In `handle_srtla_data()`, find the section where data packets are forwarded to the SRT socket. After the connection lookup (`group_find_by_addr`) succeeds and before the `sendto()` call to SLS, add the counter increments.

Look for the code path where `c` (the connection) and `g` (the group) are valid and `n` (bytes received from `recvfrom`) is available. Add:

```c
// Per-link telemetry counters
c->bytes_recv += n;
c->pkts_recv++;
clock_gettime(CLOCK_MONOTONIC, &c->last_recv_ts);
g->total_bytes_recv += n;
```

**Important:** Place this BEFORE the `sendto()` to the SRT socket, not after. This ensures the counters are updated even if the forward fails.

**Step 2: Also initialize total_bytes_recv in group_create()**

In `group_create()` (around line 161), add after the existing field initialization:

```c
g->total_bytes_recv = 0;
```

**Step 3: Verify compilation**

```bash
cd build && make
```

Expected: compiles clean.

**Step 4: Commit**

```bash
git add srtla_rec.c
git commit -m "feat: instrument handle_srtla_data with per-link byte/packet counters"
```

---

### Task 4: Implement the HTTP stats server

**Files:**
- Create: `stats_server.h`
- Create: `stats_server.c`

**Step 1: Create stats_server.h**

```c
#ifndef STATS_SERVER_H
#define STATS_SERVER_H

// Forward declaration — stats_server.c needs access to the groups linked list
// defined in srtla_rec.c. We pass it as a parameter to avoid extern globals.
typedef struct srtla_conn_group conn_group_t;

// Start the stats HTTP server on a background thread.
// Returns 0 on success, -1 on failure.
// groups_ptr: pointer to the head of the groups linked list (read-only from stats thread)
// port: TCP port to listen on (default 5080)
int stats_server_start(conn_group_t **groups_ptr, int port);

// Stop the stats server and join the thread.
void stats_server_stop(void);

#endif
```

**Step 2: Create stats_server.c**

```c
/*
 * stats_server.c — Lightweight HTTP server exposing per-link SRTLA stats.
 *
 * Runs in a pthread, reads conn_t counters written by the single-threaded
 * srtla_rec event loop. No locks needed — aligned 64-bit reads are atomic
 * on x86_64 and aarch64.
 *
 * Copyright (C) 2026 Michael Pentz / Telemy
 * Based on srtla by BELABOX (AGPL-3.0)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "stats_server.h"
#include "common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Need full type definitions from srtla_rec.c
// We re-include the struct definitions here (they are typedef'd in srtla_rec.c)
// Since srtla_rec.c doesn't have a header for its structs, we extern-reference
// what we need via the groups_ptr passed to stats_server_start().
#include "srtla_rec_types.h"

static pthread_t stats_thread;
static volatile int stats_running = 0;
static int stats_sock = -1;
static conn_group_t **groups_ref = NULL;

static void write_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) break;
        sent += n;
    }
}

static void handle_stats_request(int client_fd) {
    // Read request (we only care that something came in)
    char req[1024];
    ssize_t n = read(client_fd, req, sizeof(req) - 1);
    if (n <= 0) return;
    req[n] = '\0';

    // Only handle GET /stats
    if (strncmp(req, "GET /stats", 10) != 0) {
        const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        write_all(client_fd, resp, strlen(resp));
        return;
    }

    // Build JSON response
    char body[8192];
    int pos = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    time_t ts = time(NULL);

    pos += snprintf(body + pos, sizeof(body) - pos, "{\"ts\":%ld,\"groups\":[", (long)ts);

    conn_group_t *g = groups_ref ? *groups_ref : NULL;
    int first_group = 1;
    while (g && pos < (int)sizeof(body) - 512) {
        if (!first_group) body[pos++] = ',';
        first_group = 0;

        // Count connections
        int conn_count = 0;
        conn_t *c = g->conns;
        while (c) { conn_count++; c = c->next; }

        // Group ID as hex
        char id_hex[SRTLA_ID_LEN * 2 + 1];
        for (int i = 0; i < SRTLA_ID_LEN && i < 8; i++)
            snprintf(id_hex + i * 2, 3, "%02x", (unsigned char)g->id[i]);
        id_hex[16] = '\0';

        pos += snprintf(body + pos, sizeof(body) - pos,
            "{\"id\":\"%s\",\"conn_count\":%d,\"total_bytes\":%llu,\"connections\":[",
            id_hex, conn_count, (unsigned long long)g->total_bytes_recv);

        c = g->conns;
        int first_conn = 1;
        while (c && pos < (int)sizeof(body) - 256) {
            if (!first_conn) body[pos++] = ',';
            first_conn = 0;

            // Format address
            char addr_str[64];
            struct sockaddr_in *sin = (struct sockaddr_in *)&c->addr;
            snprintf(addr_str, sizeof(addr_str), "%s:%d",
                inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));

            // Compute staleness (ms since last packet)
            long last_ms_ago = 0;
            if (c->last_recv_ts.tv_sec > 0) {
                last_ms_ago = (now.tv_sec - c->last_recv_ts.tv_sec) * 1000 +
                              (now.tv_nsec - c->last_recv_ts.tv_nsec) / 1000000;
                if (last_ms_ago < 0) last_ms_ago = 0;
            }

            // Compute uptime
            long uptime_s = 0;
            if (c->first_seen.tv_sec > 0) {
                uptime_s = now.tv_sec - c->first_seen.tv_sec;
                if (uptime_s < 0) uptime_s = 0;
            }

            // Compute share percentage
            double share_pct = 0.0;
            if (g->total_bytes_recv > 0) {
                share_pct = (double)c->bytes_recv / (double)g->total_bytes_recv * 100.0;
            }

            pos += snprintf(body + pos, sizeof(body) - pos,
                "{\"addr\":\"%s\",\"bytes\":%llu,\"pkts\":%llu,"
                "\"share_pct\":%.1f,\"last_ms_ago\":%ld,\"uptime_s\":%ld}",
                addr_str,
                (unsigned long long)c->bytes_recv,
                (unsigned long long)c->pkts_recv,
                share_pct, last_ms_ago, uptime_s);

            c = c->next;
        }

        pos += snprintf(body + pos, sizeof(body) - pos, "]}");
        g = g->next;
    }

    pos += snprintf(body + pos, sizeof(body) - pos, "]}");

    // HTTP response
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", pos);

    write_all(client_fd, header, hlen);
    write_all(client_fd, body, pos);
}

static void *stats_thread_fn(void *arg) {
    int port = *(int *)arg;
    free(arg);

    stats_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (stats_sock < 0) {
        err("stats_server: socket failed: %s\n", strerror(errno));
        return NULL;
    }

    int opt = 1;
    setsockopt(stats_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(stats_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        err("stats_server: bind port %d failed: %s\n", port, strerror(errno));
        close(stats_sock);
        stats_sock = -1;
        return NULL;
    }

    if (listen(stats_sock, 4) < 0) {
        err("stats_server: listen failed: %s\n", strerror(errno));
        close(stats_sock);
        stats_sock = -1;
        return NULL;
    }

    info("stats_server: listening on port %d\n", port);

    while (stats_running) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(stats_sock, &fds);

        int ret = select(stats_sock + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        int client = accept(stats_sock, NULL, NULL);
        if (client < 0) continue;

        handle_stats_request(client);
        close(client);
    }

    close(stats_sock);
    stats_sock = -1;
    return NULL;
}

int stats_server_start(conn_group_t **groups_ptr, int port) {
    if (port <= 0) return 0; // disabled
    groups_ref = groups_ptr;
    stats_running = 1;

    int *port_arg = malloc(sizeof(int));
    *port_arg = port;

    if (pthread_create(&stats_thread, NULL, stats_thread_fn, port_arg) != 0) {
        err("stats_server: pthread_create failed\n");
        free(port_arg);
        return -1;
    }
    return 0;
}

void stats_server_stop(void) {
    if (!stats_running) return;
    stats_running = 0;
    if (stats_sock >= 0) {
        shutdown(stats_sock, SHUT_RDWR);
    }
    pthread_join(stats_thread, NULL);
    info("stats_server: stopped\n");
}
```

**Note on struct access:** The stats server needs to read `conn_t` and `conn_group_t` fields. Since srtla_rec.c defines these as file-local typedefs, we need to either:
- (a) Extract the struct definitions into a shared header `srtla_rec_types.h`, OR
- (b) Keep the stats code in `srtla_rec.c` as static functions

**Recommended: option (a)** — create `srtla_rec_types.h` with the struct typedefs, include it from both `srtla_rec.c` and `stats_server.c`. Move `conn_t`, `conn_group_t`, and `srtla_ack_pkt` typedefs there.

**Step 3: Create srtla_rec_types.h**

```c
#ifndef SRTLA_REC_TYPES_H
#define SRTLA_REC_TYPES_H

#include "common.h"
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>

#ifndef RECV_ACK_INT
#define RECV_ACK_INT 10
#endif

#ifndef SRTLA_ID_LEN
#define SRTLA_ID_LEN 8
#endif

typedef struct srtla_conn {
    struct srtla_conn *next;
    struct sockaddr addr;
    time_t last_rcvd;
    int recv_idx;
    uint32_t recv_log[RECV_ACK_INT];
    uint64_t bytes_recv;
    uint64_t pkts_recv;
    struct timespec first_seen;
    struct timespec last_recv_ts;
} conn_t;

typedef struct srtla_conn_group {
    struct srtla_conn_group *next;
    conn_t *conns;
    time_t created_at;
    int srt_sock;
    struct sockaddr last_addr;
    char id[SRTLA_ID_LEN];
    uint64_t total_bytes_recv;
} conn_group_t;

typedef struct {
    uint32_t type;
    uint32_t acks[RECV_ACK_INT];
} srtla_ack_pkt;

#endif
```

**Step 4: Update srtla_rec.c to include the shared header**

Remove the inline struct definitions from `srtla_rec.c` (lines ~41-61) and replace with:

```c
#include "srtla_rec_types.h"
```

**Step 5: Update stats_server.c include**

Replace the `#include "srtla_rec_types.h"` placeholder (already in the code above).

**Step 6: Verify compilation**

Update CMakeLists.txt (or Makefile) to compile `stats_server.c` and link with `-lpthread`:

```cmake
add_executable(srtla_rec srtla_rec.c common.c stats_server.c)
target_link_libraries(srtla_rec pthread)
```

```bash
cd build && cmake .. && make
```

Expected: compiles clean.

**Step 7: Commit**

```bash
git add stats_server.h stats_server.c srtla_rec_types.h srtla_rec.c CMakeLists.txt
git commit -m "feat: add HTTP stats server for per-link telemetry on port 5080"
```

---

### Task 5: Add --stats_port CLI argument and wire into main()

**Files:**
- Modify: `srtla_rec.c` (main function, around line 628)

**Step 1: Add --stats_port to argument parsing**

In `main()`, find the existing argument parsing loop. Add a new `--stats_port` option:

```c
int stats_port = 5080; // default

// In the arg parsing section:
if (!strcmp(argv[i], "--stats_port")) {
    if (++i >= argc) print_help(argv[0]);
    stats_port = atoi(argv[i]);
}
```

**Step 2: Start stats server after socket setup, before event loop**

After the epoll setup and before the `while(1)` event loop, add:

```c
// Start per-link stats HTTP server
if (stats_server_start(&groups, stats_port) != 0) {
    err("Failed to start stats server on port %d\n", stats_port);
    // Non-fatal — continue without stats
}
```

**Step 3: Stop stats server on shutdown**

At the end of `main()` or in any cleanup path, add:

```c
stats_server_stop();
```

**Step 4: Update help text**

Add to the existing usage/help output:

```c
"  --stats_port <port>  HTTP stats server port (default: 5080, 0 to disable)\n"
```

**Step 5: Verify compilation and test**

```bash
cd build && make
./srtla_rec --srtla_port 5000 --srt_hostname localhost --srt_port 4001 --stats_port 5080 &
curl http://localhost:5080/stats
```

Expected: `{"ts":...,"groups":[]}` (empty groups, no active connections).

**Step 6: Commit**

```bash
git add srtla_rec.c
git commit -m "feat: add --stats_port CLI arg, wire stats server into main loop"
```

**Step 7: Push to GitHub**

```bash
git push origin main
```

---

## Lane 2: Docker Image

### Task 6: Fork srtla-receiver and build custom image

**Manual step** — GitHub fork + Dockerfile changes.

**Step 1: Fork on GitHub**

Go to https://github.com/OpenIRL/srtla-receiver and fork to `telemyapp/srtla-receiver`.

**Step 2: Clone locally**

```bash
cd /e/Code/telemyapp
git clone https://github.com/telemyapp/srtla-receiver.git srtla-receiver-fork
cd srtla-receiver-fork
```

**Step 3: Update Dockerfile to use our srtla fork**

Find the line that clones the srtla repo (likely `git clone https://github.com/OpenIRL/srtla.git`). Change to:

```dockerfile
RUN git clone https://github.com/telemyapp/srtla.git /build/srtla
```

**Step 4: Expose port 5080 in Dockerfile**

Add after the existing `EXPOSE` lines:

```dockerfile
EXPOSE 5080/tcp
```

**Step 5: Update conf/supervisord.conf**

Change the srtla command to include `--stats_port=5080`:

```ini
[program:srtla]
priority=150
command=/bin/sh -c 'sleep 3 && /bin/logprefix /usr/local/bin/srtla_rec --srtla_port=5000 --srt_hostname=localhost --srt_port=4001 --stats_port=5080'
user=srtla
autorestart=true
autostart=true
stdout_logfile=/dev/stdout
stdout_logfile_maxbytes=0
stderr_logfile=/dev/stderr
stderr_logfile_maxbytes=0
```

**Step 6: Build and test locally**

```bash
docker build -t ghcr.io/telemyapp/srtla-receiver:latest .
docker run -p 5080:5080 -p 5000:5000/udp -p 4000:4000/udp -p 4001:4001/udp -p 8080:8080 ghcr.io/telemyapp/srtla-receiver:latest
# In another terminal:
curl http://localhost:5080/stats
```

Expected: `{"ts":...,"groups":[]}`.

**Step 7: Add GitHub Actions CI**

Create `.github/workflows/build-image.yml`:

```yaml
name: Build and Push Docker Image
on:
  push:
    branches: [main]

jobs:
  build:
    runs-on: ubuntu-latest
    permissions:
      contents: read
      packages: write
    steps:
      - uses: actions/checkout@v4
      - uses: docker/setup-qemu-action@v3
      - uses: docker/setup-buildx-action@v3
      - uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}
      - uses: docker/build-push-action@v5
        with:
          context: .
          platforms: linux/amd64,linux/arm64
          push: true
          tags: |
            ghcr.io/telemyapp/srtla-receiver:latest
            ghcr.io/telemyapp/srtla-receiver:${{ github.sha }}
```

**Step 8: Commit and push**

```bash
git add Dockerfile conf/supervisord.conf .github/workflows/build-image.yml
git commit -m "feat: custom srtla-receiver with per-link stats on port 5080"
git push origin main
```

Wait for GitHub Actions to build the image.

---

## Lane 3: Relay Provisioning

### Task 7: Update relay-user-data.sh to use custom Docker image

**Files:**
- Modify: `telemy-v0.0.4/aegis-control-plane/scripts/relay-user-data.sh:42-55`

**Step 1: Replace docker-compose fetch with inline compose file**

Replace the existing curl fetch of `docker-compose.prod.yml` (lines 42-55) with an inline compose file that uses our custom image:

In `relay-user-data.sh`, replace:

```bash
# Fetch docker-compose from OpenIRL
SRTLA_RECEIVER_COMMIT_SHA="e2fe790dbb4c286e6506cf996f1de32bbb3764d2"
curl -sL "https://raw.githubusercontent.com/OpenIRL/srtla-receiver/${SRTLA_RECEIVER_COMMIT_SHA}/docker-compose.prod.yml" \
  -o docker-compose.yml
```

With:

```bash
# Write docker-compose with custom srtla-receiver image (per-link stats)
cat > docker-compose.yml << 'COMPOSEEOF'
services:
  sls-management-ui:
    image: ghcr.io/openirl/sls-management-ui:latest
    container_name: sls-management-ui
    restart: unless-stopped
    environment:
      REACT_APP_BASE_URL: "${APP_URL}"
      REACT_APP_SRT_PLAYER_PORT: "${SRT_PLAYER_PORT:-4000}"
      REACT_APP_SRT_SENDER_PORT: "${SRT_SENDER_PORT:-4001}"
      REACT_APP_SLS_STATS_PORT: "${SLS_STATS_PORT:-8080}"
      REACT_APP_SRTLA_PORT: "${SRTLA_PORT:-5000}"
    ports:
      - "${SLS_MGNT_PORT}:3000"
    logging:
      driver: "json-file"
      options:
        max-size: "10m"
        max-file: "3"

  receiver:
    image: ghcr.io/telemyapp/srtla-receiver:latest
    container_name: srtla-receiver
    restart: unless-stopped
    ports:
      - "${SLS_STATS_PORT}:8080/tcp"
      - "${SRTLA_PORT}:5000/udp"
      - "${SRT_SENDER_PORT}:4001/udp"
      - "${SRT_PLAYER_PORT}:4000/udp"
      - "5080:5080/tcp"
    volumes:
      - ./data:/var/lib/sls
    logging:
      driver: "json-file"
      options:
        max-size: "10m"
        max-file: "3"
COMPOSEEOF
```

**Step 2: Add port 5080 to .env comment for clarity**

Update the `.env` heredoc comment (no functional change, just documentation):

```bash
cat > .env << 'ENVEOF'
SRTLA_PORT=5000
SRT_SENDER_PORT=4001
SRT_PLAYER_PORT=4000
SLS_MGNT_PORT=3000
SLS_STATS_PORT=8090
APP_URL=http://localhost
# Per-link stats exposed on port 5080 (hardcoded in compose, not variable)
ENVEOF
```

**Step 3: Commit**

```bash
cd /e/Code/telemyapp/telemy-v0.0.4
git add aegis-control-plane/scripts/relay-user-data.sh
git commit -m "feat: use custom srtla-receiver image with per-link stats port 5080"
```

---

### Task 8: Add port 5080 to relay security group in aws.go

**Files:**
- Modify: `telemy-v0.0.4/aegis-control-plane/internal/relay/aws.go`

**Step 1: Find the security group ingress rules**

Search for where TCP/UDP ports are added to the relay security group. The SG ID is `sg-0da8cf50c2fd72518`.

**Note:** The current code passes `SecurityGroup` as a pre-existing SG ID from config — ports are configured statically in the SG, not dynamically in Go code. So this is a **manual AWS Console/CLI step**:

```bash
aws ec2 authorize-security-group-ingress \
  --group-id sg-0da8cf50c2fd72518 \
  --protocol tcp --port 5080 --cidr 0.0.0.0/0 \
  --region us-west-2
```

**Step 2: Document in relay-user-data.sh header**

Update the port map comment at the top of `relay-user-data.sh`:

```bash
# Port map:
#   UDP 5000  SRTLA bonded ingest (IRL Pro connects here)
#   UDP 4000  SRT player output (OBS connects here)
#   UDP 4001  SRT direct sender (non-bonded fallback)
#   TCP 3000  Management UI
#   TCP 5080  Per-link stats API (srtla_rec HTTP)
#   TCP 8090  Backend API (remapped from default 8080 to avoid control plane conflict)
```

**Step 3: Commit**

```bash
git add aegis-control-plane/scripts/relay-user-data.sh
git commit -m "docs: add port 5080 to relay port map, note SG rule requirement"
```

---

## Lane 4: C++ Plugin

### Task 9: Add PerLinkStats and PerLinkSnapshot structs

**Files:**
- Modify: `telemy-v0.0.4/obs-plugin/src/relay_client.h:35` (after RelayStats struct)

**Step 1: Add structs after RelayStats (line 35)**

```cpp
struct PerLinkStats {
    std::string addr;
    uint64_t bytes = 0;
    uint64_t pkts = 0;
    double share_pct = 0.0;
    uint32_t last_ms_ago = 0;
    uint32_t uptime_s = 0;
};

struct PerLinkSnapshot {
    bool available = false;
    int conn_count = 0;
    std::vector<PerLinkStats> links;
};
```

**Step 2: Add include for vector (if not present)**

Add at top of `relay_client.h`:

```cpp
#include <vector>
```

**Step 3: Add polling method and state to RelayClient**

After the existing `PollRelayStats` / `CurrentStats` declarations (line 70), add:

```cpp
// Per-link stats polling (srtla_rec fork)
void PollPerLinkStats(const std::string& relay_ip);
PerLinkSnapshot CurrentPerLinkStats() const;
```

After the existing `stats_mutex_` (line 88), add:

```cpp
PerLinkSnapshot  per_link_;
mutable std::mutex per_link_mutex_;
```

**Step 4: Verify compilation**

```bash
cd obs-plugin && cmake --build build-obs-cef --config Release
```

Expected: compiles (declarations only, no implementations yet).

**Step 5: Commit**

```bash
git add obs-plugin/src/relay_client.h
git commit -m "feat: add PerLinkStats/PerLinkSnapshot structs and method declarations"
```

---

### Task 10: Implement PollPerLinkStats in relay_client.cpp

**Files:**
- Modify: `telemy-v0.0.4/obs-plugin/src/relay_client.cpp:417` (after CurrentStats)

**Step 1: Add QJsonArray include**

At the top of `relay_client.cpp`, add after existing Qt includes (line 7):

```cpp
#include <QJsonArray>
```

**Step 2: Implement PollPerLinkStats()**

Add after `CurrentStats()` (line 417):

```cpp
void RelayClient::PollPerLinkStats(const std::string& relay_ip)
{
    if (relay_ip.empty()) {
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_.available = false;
        per_link_.links.clear();
        return;
    }

    std::string host_port = relay_ip + ":5080";
    std::wstring host_w(host_port.begin(), host_port.end());
    std::wstring path_w = L"/stats";

    HttpResponse resp;
    try {
        resp = http_.Get(host_w, path_w);
    } catch (const std::exception& e) {
        blog(LOG_DEBUG, "[aegis-relay] per-link stats http error: %s", e.what());
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_.available = false;
        return;
    }

    if (resp.status_code != 200 || resp.body.empty()) {
        blog(LOG_DEBUG, "[aegis-relay] per-link stats failed: status=%lu",
             resp.status_code);
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_.available = false;
        return;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray(resp.body.c_str(), static_cast<int>(resp.body.size())), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_.available = false;
        return;
    }

    QJsonObject root = doc.object();
    QJsonArray groupsArr = root.value("groups").toArray();

    PerLinkSnapshot snap;
    snap.available = true;

    // We expect one group (single stream). Take the first.
    if (!groupsArr.isEmpty()) {
        QJsonObject group = groupsArr[0].toObject();
        snap.conn_count = group.value("conn_count").toInt(0);
        QJsonArray connsArr = group.value("connections").toArray();

        for (int i = 0; i < connsArr.size(); ++i) {
            QJsonObject c = connsArr[i].toObject();
            PerLinkStats link;
            link.addr = c.value("addr").toString().toStdString();
            link.bytes = static_cast<uint64_t>(c.value("bytes").toDouble(0));
            link.pkts = static_cast<uint64_t>(c.value("pkts").toDouble(0));
            link.share_pct = c.value("share_pct").toDouble(0.0);
            link.last_ms_ago = static_cast<uint32_t>(c.value("last_ms_ago").toInt(0));
            link.uptime_s = static_cast<uint32_t>(c.value("uptime_s").toInt(0));
            snap.links.push_back(std::move(link));
        }
    }

    {
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_ = std::move(snap);
    }

    blog(LOG_DEBUG, "[aegis-relay] per-link stats ok: conn_count=%d links=%zu",
         per_link_.conn_count, per_link_.links.size());
}

PerLinkSnapshot RelayClient::CurrentPerLinkStats() const
{
    std::lock_guard<std::mutex> lk(per_link_mutex_);
    return per_link_;
}
```

**Step 3: Verify compilation**

```bash
cd obs-plugin && cmake --build build-obs-cef --config Release
```

Expected: compiles clean.

**Step 4: Commit**

```bash
git add obs-plugin/src/relay_client.cpp
git commit -m "feat: implement PollPerLinkStats from srtla_rec HTTP endpoint"
```

---

### Task 11: Wire per-link stats into dock state JSON

**Files:**
- Modify: `telemy-v0.0.4/obs-plugin/src/metrics_collector.h:94-96`
- Modify: `telemy-v0.0.4/obs-plugin/src/metrics_collector.cpp:407-486`
- Modify: `telemy-v0.0.4/obs-plugin/src/obs_plugin_entry.cpp:981-995`

**Step 1: Update BuildStatusSnapshotJson signature**

In `metrics_collector.h`, add `PerLinkSnapshot` forward declaration (after line 19):

```cpp
struct PerLinkSnapshot;
```

Update the method signature (line 89-96) to accept per-link data:

```cpp
std::string BuildStatusSnapshotJson(
    const std::string& mode,
    const std::string& health,
    const std::string& relay_status,
    const std::string& relay_region,
    const aegis::RelaySession* relay_session = nullptr,
    const aegis::RelayStats* relay_stats = nullptr,
    const aegis::PerLinkSnapshot* per_link_stats = nullptr
) const;
```

**Step 2: Add per-link JSON serialization**

In `metrics_collector.cpp`, update the function signature (line 407) to match, then add per-link serialization after the relay stats block (after line 486, before the multistream_outputs block):

```cpp
// ── Per-link relay telemetry (from srtla_rec fork) ───────────────
if (per_link_stats && per_link_stats->available && !per_link_stats->links.empty()) {
    os << "\"relay_per_link_available\":true,";
    os << "\"relay_conn_count\":" << per_link_stats->conn_count << ",";
    os << "\"relay_links\":[";
    for (size_t i = 0; i < per_link_stats->links.size(); ++i) {
        if (i > 0) os << ",";
        const auto& link = per_link_stats->links[i];
        os << "{";
        os << "\"addr\":\"" << JsonEscape(link.addr) << "\",";
        os << "\"bytes\":" << link.bytes << ",";
        os << "\"pkts\":" << link.pkts << ",";

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", link.share_pct);
        os << "\"share_pct\":" << buf << ",";

        os << "\"last_ms_ago\":" << link.last_ms_ago << ",";
        os << "\"uptime_s\":" << link.uptime_s;
        os << "}";
    }
    os << "],";
} else {
    os << "\"relay_per_link_available\":false,";
}
```

**Step 3: Wire polling into the tick callback**

In `obs_plugin_entry.cpp`, in `EmitCurrentStatusSnapshotToDock()` (around line 984), after the existing SLS stats polling block, add per-link polling:

```cpp
// Poll per-link stats on the same cadence as SLS stats
const aegis::PerLinkSnapshot* per_link_ptr = nullptr;
aegis::PerLinkSnapshot per_link_stats;
if (g_relay && g_relay->HasActiveSession() && relay_session_ptr) {
    // Shares the same 4-tick counter as SLS stats (polls every ~2s)
    // The counter was already incremented and reset above
    static int per_link_poll_counter = 0;
    if (++per_link_poll_counter >= 4) {
        per_link_poll_counter = 0;
        g_relay->PollPerLinkStats(relay_session_ptr->public_ip);
    }
    per_link_stats = g_relay->CurrentPerLinkStats();
    per_link_ptr = &per_link_stats;
}
```

Update the `BuildStatusSnapshotJson` call (line 994-995) to include per-link:

```cpp
std::string json =
    g_metrics.BuildStatusSnapshotJson(mode, health, relay_status, relay_region,
                                       relay_session_ptr, relay_stats_ptr, per_link_ptr);
```

**Step 4: Add include for PerLinkSnapshot in obs_plugin_entry.cpp**

The `relay_client.h` include should already be present. Verify that `PerLinkSnapshot` is accessible (it's in the `aegis` namespace in `relay_client.h`).

**Step 5: Verify compilation**

```bash
cd obs-plugin && cmake --build build-obs-cef --config Release
```

Expected: compiles clean.

**Step 6: Commit**

```bash
git add obs-plugin/src/metrics_collector.h obs-plugin/src/metrics_collector.cpp obs-plugin/src/obs_plugin_entry.cpp
git commit -m "feat: wire per-link relay stats into dock state JSON snapshot"
```

---

## Lane 5: Dock UI

### Task 12: Bridge JS — per-link pass-through

**Files:**
- Modify: `telemy-v0.0.4/obs-plugin/dock/aegis-dock-bridge.js:359` (after statsAvailable line in relay object)

**Step 1: Add per-link fields to getState().relay**

After the existing `statsAvailable: !!snap.relay_stats_available,` (line 359), add:

```javascript
          // Per-link telemetry (from srtla_rec fork)
          perLinkAvailable: !!snap.relay_per_link_available,
          connCount: snap.relay_conn_count || 0,
          links: (snap.relay_links || []).map(function(l) {
            return {
              addr: l.addr || "",
              bytes: l.bytes || 0,
              pkts: l.pkts || 0,
              sharePct: l.share_pct || 0,
              lastMsAgo: l.last_ms_ago || 0,
              uptimeS: l.uptime_s || 0,
            };
          }),
```

**Step 2: Commit**

```bash
git add obs-plugin/dock/aegis-dock-bridge.js
git commit -m "feat: pass per-link relay stats through dock bridge"
```

---

### Task 13: Add simulated per-link data to dock-preview

**Files:**
- Modify: `telemy-v0.0.4/obs-plugin/dock/use-simulated-state.js:80-95` (relay section)

**Step 1: Add per-link simulated data to the relay object**

In `use-simulated-state.js`, in the `relay` object inside the `useMemo` state builder (around line 80-95), add after the existing `statsAvailable` field (add it if not present):

```javascript
      statsAvailable: simRelayActive,
      ingestBitrateKbps: simRelayActive ? sim1 + sim2 : 0,
      rttMs: simRelayActive ? 42 : null,
      relayLatencyMs: simRelayActive ? 120 : null,
      pktLoss: simRelayActive ? 234 : 0,
      pktDrop: simRelayActive ? 3 : 0,
      lossRate: simRelayActive ? 2 : 0,
      recvRateMbps: simRelayActive ? (sim1 + sim2) / 1000 : null,
      bandwidthMbps: simRelayActive ? 12.0 : null,
      uptimeSeconds: simRelayActive ? elapsed : 0,
      // Per-link simulated data
      perLinkAvailable: simRelayActive,
      connCount: simRelayActive ? 2 : 0,
      links: simRelayActive ? [
        { addr: "192.168.1.105:45032", bytes: Math.floor(sim1 * elapsed * 0.125), pkts: Math.floor(sim1 * elapsed * 0.125 / 1350), sharePct: sim1 / (sim1 + sim2) * 100, lastMsAgo: 12, uptimeS: elapsed },
        { addr: "172.58.12.99:38201", bytes: Math.floor(sim2 * elapsed * 0.125), pkts: Math.floor(sim2 * elapsed * 0.125 / 1350), sharePct: sim2 / (sim1 + sim2) * 100, lastMsAgo: 8, uptimeS: elapsed },
      ] : [],
```

**Step 2: Commit**

```bash
git add obs-plugin/dock/use-simulated-state.js
git commit -m "feat: add simulated per-link relay data for dock-preview"
```

---

### Task 14: Add IP label heuristic utility

**Files:**
- Modify: `telemy-v0.0.4/obs-plugin/dock/utils.js`

**Step 1: Add classifyLinkAddr function**

Add at the end of `utils.js`:

```javascript
/** Classify a link address as WiFi/LAN or Cellular based on IP range heuristic */
export function classifyLinkAddr(addr) {
  if (!addr) return { label: "Link", type: "unknown" };
  var ip = addr.split(":")[0];
  if (ip.startsWith("10.") || ip.startsWith("192.168.") ||
      (ip.startsWith("172.") && (function() {
        var second = parseInt(ip.split(".")[1], 10);
        return second >= 16 && second <= 31;
      })())) {
    return { label: "WiFi", type: "wifi" };
  }
  return { label: "Cell", type: "cellular" };
}
```

**Step 2: Commit**

```bash
git add obs-plugin/dock/utils.js
git commit -m "feat: add classifyLinkAddr IP heuristic for per-link labels"
```

---

### Task 15: Dock UI — per-link throughput bars and connection count

**Files:**
- Modify: `telemy-v0.0.4/obs-plugin/dock/aegis-dock.jsx:1-32` (imports)
- Modify: `telemy-v0.0.4/obs-plugin/dock/aegis-dock.jsx:1262-1274` (Bitrate section)
- Modify: `telemy-v0.0.4/obs-plugin/dock/aegis-dock.jsx:1340-1345` (Relay section badge)

**Step 1: Add classifyLinkAddr import**

In `aegis-dock.jsx`, add `classifyLinkAddr` to the utils import (line 12):

```javascript
import {
  genRequestId, formatTime, parseHexColor, toRgba, isLightColor,
  normalizeOptionalHexColor, getDefaultRuleBgColor, normalizeIntent,
  inferIntentFromName, normalizeSceneName, findBestSceneIdForRule,
  mapRelayStatusForUi, loadSceneIntentLinks, loadSceneIntentLinkNames,
  findSceneIdByName, normalizeLinkMap, loadAutoSceneRules,
  normalizeAutoSceneRulesValue, cefCopyToClipboard, classifyLinkAddr
} from "./utils.js";
```

**Step 2: Add per-link throughput calculation ref**

Inside the `AegisDock` component, after the existing refs (around line 38), add:

```javascript
const prevLinkBytesRef = useRef({});
const prevLinkTsRef = useRef(0);
```

**Step 3: Compute per-link throughput from cumulative bytes**

After the existing bitrate calculations (around line 467), add:

```javascript
  // Per-link throughput from cumulative byte deltas
  const perLinkThroughputs = useMemo(() => {
    if (!relay.perLinkAvailable || !relay.links || relay.links.length === 0) return [];
    const now = Date.now();
    const dt = (now - prevLinkTsRef.current) / 1000;
    const prev = prevLinkBytesRef.current;
    const results = relay.links.map(link => {
      const prevBytes = prev[link.addr] || 0;
      const kbps = dt > 0.5 && prevBytes > 0
        ? Math.max(0, ((link.bytes - prevBytes) / dt) * 8 / 1000)
        : 0;
      return { ...link, kbps, ...classifyLinkAddr(link.addr) };
    });
    // Store current values for next delta
    const nextPrev = {};
    relay.links.forEach(l => { nextPrev[l.addr] = l.bytes; });
    prevLinkBytesRef.current = nextPrev;
    prevLinkTsRef.current = now;
    return results;
  }, [relay.perLinkAvailable, relay.links]);
```

**Step 4: Update the Bitrate section to show per-link bars**

Replace the existing per-link bar block (lines 1265-1274):

```jsx
          {relayActive && conns.length >= 2 ? (
            <>
              <BitrateBar value={animLink1} max={maxPerLink} color="#2d7aed"
                label={conns[0]?.name?.split(" \u2014 ")[0] || "LINK 1"} />
              <BitrateBar value={animLink2} max={maxPerLink} color="#8b5cf6"
                label={conns[1]?.name?.split(" \u2014 ")[0] || "LINK 2"} />
              <BitrateBar value={animBonded} max={maxBonded} color="#2ea043" label="BONDED" />
              <BitrateBar value={animRelayBonded} max={maxBonded} color="#5ba3f5" label="AWS RELAY INGEST" />
            </>
          ) : null}
```

With:

```jsx
          {/* Per-link bars from srtla_rec stats */}
          {relayActive && perLinkThroughputs.length > 0 ? (
            <>
              {perLinkThroughputs.map((link, i) => (
                <div key={link.addr} style={{ opacity: link.lastMsAgo > 3000 ? 0.4 : 1 }}>
                  <BitrateBar
                    value={link.kbps}
                    max={Math.max(relayBondedKbps, 6000)}
                    color={i === 0 ? "#2d7aed" : i === 1 ? "#8b5cf6" : "#e05d44"}
                    label={`${link.label}  ${Math.round(link.sharePct)}%`}
                  />
                </div>
              ))}
              <BitrateBar value={relayBondedKbps} max={Math.max(relayBondedKbps * 1.5, 10000)} color="#2ea043" label="BONDED" />
            </>
          ) : relayActive && conns.length >= 2 ? (
            <>
              <BitrateBar value={animLink1} max={maxPerLink} color="#2d7aed"
                label={conns[0]?.name?.split(" \u2014 ")[0] || "LINK 1"} />
              <BitrateBar value={animLink2} max={maxPerLink} color="#8b5cf6"
                label={conns[1]?.name?.split(" \u2014 ")[0] || "LINK 2"} />
              <BitrateBar value={animBonded} max={maxBonded} color="#2ea043" label="BONDED" />
              <BitrateBar value={animRelayBonded} max={maxBonded} color="#5ba3f5" label="AWS RELAY INGEST" />
            </>
          ) : null}
```

**Step 5: Add connection count badge to Relay section**

In the Relay section header (around line 1344), update the badge to show link count when active:

Replace:
```jsx
badge={!relayLicensed ? "PRO" : relayActive ? relayStatusUi.toUpperCase() : "OFF"}
```

With:
```jsx
badge={!relayLicensed ? "PRO" : relayActive ? (relay.connCount > 0 ? `${relay.connCount} LINK${relay.connCount !== 1 ? "S" : ""}` : relayStatusUi.toUpperCase()) : "OFF"}
```

**Step 6: Commit**

```bash
git add obs-plugin/dock/aegis-dock.jsx
git commit -m "feat: per-link throughput bars and connection count badge in dock UI"
```

---

### Task 16: Build dock bundle and test in dock-preview

**Step 1: Build esbuild bundle**

```bash
cd /e/Code/telemyapp/telemy-v0.0.4/obs-plugin/dock && NODE_PATH=../../../dock-preview/node_modules npx esbuild aegis-dock-entry.jsx --bundle --format=iife --jsx=automatic --outfile=aegis-dock-app.js --target=es2020 --minify
```

Expected: `aegis-dock-app.js` generated, no build errors.

**Step 2: Test in dock-preview**

```bash
cd /e/Code/telemyapp/dock-preview && npx vite --port 5199
```

Open http://localhost:5199. Verify:
- Per-link throughput bars appear in the Bitrate section (WiFi + Cell)
- Traffic share percentages shown (e.g. "WiFi 60%" / "Cell 40%")
- Connection count badge shows "2 LINKS" on the Relay section header
- Bars update every ~3 seconds with simulated data
- Bonded bar still shows below per-link bars

**Step 3: Commit bundle**

```bash
git add obs-plugin/dock/aegis-dock-app.js
git commit -m "build: rebuild dock bundle with per-link telemetry UI"
```

---

## Verification: E2E Test

After all lanes complete:

1. Deploy updated control plane binary (with updated relay-user-data.sh)
2. Manually add port 5080 TCP to relay SG (`sg-0da8cf50c2fd72518`)
3. Build and deploy `aegis-obs-plugin.dll` to OBS
4. Copy updated dock assets to OBS data dir
5. Activate relay from dock UI
6. Start streaming from IRL Pro (WiFi + cellular bonded) to relay
7. Check OBS logs for `[aegis-relay] per-link stats ok: conn_count=2 links=2`
8. Verify dock shows per-link bars with individual throughputs
9. Verify traffic share percentages reflect IRL Pro weight settings
10. Disconnect one link in IRL Pro — verify dock shows connection count drop to 1
11. Stop stream — verify per-link bars hide when no connections
