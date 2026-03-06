# Per-User Relay DNS Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add persistent per-user relay DNS subdomains (`{slug}.relay.telemyapp.com`) so IRL Pro users configure ingest once and never change it.

**Architecture:** Each user gets a permanent 6-char alphanumeric slug in the DB. On relay start, the Go control plane creates a Cloudflare A record pointing the slug subdomain to the EC2 IP. On relay stop, it deletes the record. The C++ plugin parses the hostname from the API response, and the dock UI displays it as the ingest URL.

**Tech Stack:** Go (control plane), PostgreSQL (DB), Cloudflare API v4 (DNS), C++ (OBS plugin), React JSX (dock UI)

---

## Prerequisites

- EC2 env vars already configured: `CLOUDFLARE_DNS_TOKEN`, `CLOUDFLARE_ZONE_ID`
- Design doc: `docs/plans/2026-03-05-per-user-relay-dns-design.md`

---

### Task 1: DB Migration - Add relay_slug Column

**Files:**
- Create: `aegis-control-plane/migrations/003_add_relay_slug.sql`

**Step 1: Write the migration SQL file**

```sql
-- 003_add_relay_slug.sql
-- Adds a permanent relay slug to each user for DNS subdomain routing.

ALTER TABLE users ADD COLUMN relay_slug VARCHAR(8);

-- Backfill existing users with random 6-char slugs
UPDATE users SET relay_slug = substr(md5(random()::text), 1, 6) WHERE relay_slug IS NULL;

-- Now enforce NOT NULL + UNIQUE
ALTER TABLE users ALTER COLUMN relay_slug SET NOT NULL;
ALTER TABLE users ADD CONSTRAINT users_relay_slug_unique UNIQUE (relay_slug);
```

**Step 2: Run the migration on EC2**

```bash
ssh -i ~/.ssh/id_server_new ec2-user@52.13.2.122
sudo -u postgres psql aegis -f /tmp/003_add_relay_slug.sql
```

Upload the file first:
```bash
scp -i ~/.ssh/id_server_new aegis-control-plane/migrations/003_add_relay_slug.sql ec2-user@52.13.2.122:/tmp/
```

**Step 3: Verify the migration**

```bash
sudo -u postgres psql aegis -c "\d users"
sudo -u postgres psql aegis -c "SELECT id, relay_slug FROM users;"
```

Expected: All users have a 6-char slug, column is NOT NULL with UNIQUE constraint.

**Step 4: Commit**

```bash
git add aegis-control-plane/migrations/003_add_relay_slug.sql
git commit -m "feat(db): add relay_slug column to users table"
```

---

### Task 2: Store - Add GetUserRelaySlug Method

**Files:**
- Modify: `aegis-control-plane/internal/store/store.go` (add method at bottom)

**Step 1: Write the store method**

Add this method to the bottom of `store.go` (before the `strPtr` helper):

```go
func (s *Store) GetUserRelaySlug(ctx context.Context, userID string) (string, error) {
	var slug string
	err := s.db.QueryRow(ctx, `SELECT relay_slug FROM users WHERE id = $1`, userID).Scan(&slug)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return "", ErrNotFound
		}
		return "", err
	}
	return slug, nil
}
```

**Step 2: Add GetUserRelaySlug to the api.Store interface**

In `aegis-control-plane/internal/api/router.go`, add to the `Store` interface (around line 21-30):

```go
GetUserRelaySlug(ctx context.Context, userID string) (string, error)
```

**Step 3: Verify build**

```bash
cd E:/Code/telemyapp/telemy-v0.0.4/aegis-control-plane && go build ./...
```

Expected: Clean build, no errors.

**Step 4: Commit**

```bash
git add aegis-control-plane/internal/store/store.go aegis-control-plane/internal/api/router.go
git commit -m "feat(store): add GetUserRelaySlug for DNS subdomain lookup"
```

---

### Task 3: Cloudflare DNS Client Package

**Files:**
- Create: `aegis-control-plane/internal/dns/cloudflare.go`

**Step 1: Write the Cloudflare DNS client**

```go
package dns

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
)

type Client struct {
	zoneID    string
	apiToken  string
	baseDomain string // e.g. "relay.telemyapp.com"
	httpClient *http.Client
}

func NewClient() *Client {
	return &Client{
		zoneID:     os.Getenv("CLOUDFLARE_ZONE_ID"),
		apiToken:   os.Getenv("CLOUDFLARE_DNS_TOKEN"),
		baseDomain: envOr("CLOUDFLARE_RELAY_DOMAIN", "relay.telemyapp.com"),
		httpClient: &http.Client{},
	}
}

func (c *Client) Enabled() bool {
	return c.zoneID != "" && c.apiToken != ""
}

func (c *Client) FQDN(slug string) string {
	return slug + "." + c.baseDomain
}

// CreateOrUpdateRecord upserts an A record for {slug}.relay.telemyapp.com -> ip.
func (c *Client) CreateOrUpdateRecord(slug, ip string) error {
	if !c.Enabled() {
		log.Printf("dns: skipping record create — cloudflare not configured")
		return nil
	}
	fqdn := c.FQDN(slug)

	// Check if record exists
	recordID, err := c.findRecord(fqdn)
	if err != nil {
		return fmt.Errorf("dns: find record %s: %w", fqdn, err)
	}

	body, _ := json.Marshal(map[string]any{
		"type":    "A",
		"name":    fqdn,
		"content": ip,
		"ttl":     60,
		"proxied": false,
	})

	var method, url string
	if recordID != "" {
		method = http.MethodPut
		url = fmt.Sprintf("https://api.cloudflare.com/client/v4/zones/%s/dns_records/%s", c.zoneID, recordID)
	} else {
		method = http.MethodPost
		url = fmt.Sprintf("https://api.cloudflare.com/client/v4/zones/%s/dns_records", c.zoneID)
	}

	req, _ := http.NewRequest(method, url, bytes.NewReader(body))
	req.Header.Set("Authorization", "Bearer "+c.apiToken)
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("dns: %s %s: %w", method, fqdn, err)
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 300 {
		b, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("dns: %s %s HTTP %d: %s", method, fqdn, resp.StatusCode, string(b))
	}
	log.Printf("dns: upserted A record %s -> %s", fqdn, ip)
	return nil
}

// DeleteRecord removes the A record for {slug}.relay.telemyapp.com.
func (c *Client) DeleteRecord(slug string) error {
	if !c.Enabled() {
		log.Printf("dns: skipping record delete — cloudflare not configured")
		return nil
	}
	fqdn := c.FQDN(slug)
	recordID, err := c.findRecord(fqdn)
	if err != nil {
		return fmt.Errorf("dns: find record %s: %w", fqdn, err)
	}
	if recordID == "" {
		return nil // nothing to delete
	}

	url := fmt.Sprintf("https://api.cloudflare.com/client/v4/zones/%s/dns_records/%s", c.zoneID, recordID)
	req, _ := http.NewRequest(http.MethodDelete, url, nil)
	req.Header.Set("Authorization", "Bearer "+c.apiToken)

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("dns: DELETE %s: %w", fqdn, err)
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 300 {
		b, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("dns: DELETE %s HTTP %d: %s", fqdn, resp.StatusCode, string(b))
	}
	log.Printf("dns: deleted A record %s", fqdn)
	return nil
}

func (c *Client) findRecord(fqdn string) (string, error) {
	url := fmt.Sprintf("https://api.cloudflare.com/client/v4/zones/%s/dns_records?type=A&name=%s", c.zoneID, fqdn)
	req, _ := http.NewRequest(http.MethodGet, url, nil)
	req.Header.Set("Authorization", "Bearer "+c.apiToken)

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	var result struct {
		Result []struct {
			ID string `json:"id"`
		} `json:"result"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return "", err
	}
	if len(result.Result) > 0 {
		return result.Result[0].ID, nil
	}
	return "", nil
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
```

**Step 2: Verify build**

```bash
cd E:/Code/telemyapp/telemy-v0.0.4/aegis-control-plane && go build ./...
```

Expected: Clean build.

**Step 3: Commit**

```bash
git add aegis-control-plane/internal/dns/cloudflare.go
git commit -m "feat(dns): add Cloudflare DNS client for relay subdomain management"
```

---

### Task 4: Wire DNS Into Relay Start/Stop Handlers

**Files:**
- Modify: `aegis-control-plane/internal/api/router.go` (add dns field to Server)
- Modify: `aegis-control-plane/internal/api/handlers.go` (call DNS after activate/deprovision)

**Step 1: Add dns.Client to Server struct**

In `router.go`:

1. Add import: `"github.com/telemyapp/aegis-control-plane/internal/dns"`
2. Add field to `Server` struct: `dns *dns.Client`
3. Update `NewRouter` to accept and store it:

```go
func NewRouter(cfg config.Config, st Store, prov relay.Provisioner, dnsClient *dns.Client) http.Handler {
	s := &Server{cfg: cfg, store: st, provisioner: prov, dns: dnsClient}
```

**Step 2: Wire DNS create into handleRelayStart**

In `handlers.go`, after the successful `ActivateProvisionedSession` call (around line 160, after `sess = activatedSess`), add:

```go
		// Create/update DNS record for relay subdomain
		if slug, slugErr := s.store.GetUserRelaySlug(r.Context(), userID); slugErr == nil && slug != "" {
			if dnsErr := s.dns.CreateOrUpdateRecord(slug, prov.PublicIP); dnsErr != nil {
				log.Printf("dns: create record failed session_id=%s slug=%s err=%v", sess.ID, slug, dnsErr)
			} else {
				sess.RelayHostname = s.dns.FQDN(slug)
			}
		}
```

**Step 3: Wire DNS delete into handleRelayStop**

In `handlers.go`, after the successful `Deprovision` call (around line 253, after the metrics logging), add:

```go
		// Delete DNS record for relay subdomain
		if slug, slugErr := s.store.GetUserRelaySlug(r.Context(), userID); slugErr == nil && slug != "" {
			if dnsErr := s.dns.DeleteRecord(slug); dnsErr != nil {
				log.Printf("dns: delete record failed session_id=%s slug=%s err=%v", curr.ID, slug, dnsErr)
			}
		}
```

**Step 4: Add RelayHostname to toSessionResponse**

In `handlers.go`, in the `toSessionResponse` function (line 376), add `relay_hostname` to the relay map:

```go
func toSessionResponse(sess *model.Session) map[string]any {
	relayMap := map[string]any{
		"public_ip": sess.PublicIP,
		"srt_port":  sess.SRTPort,
		"ws_url":    sess.WSURL,
	}
	if sess.RelayHostname != "" {
		relayMap["relay_hostname"] = sess.RelayHostname
	}
	resp := map[string]any{
		"session_id": sess.ID,
		"status":     string(sess.Status),
		"region":     sess.Region,
		"relay":      relayMap,
		"credentials": map[string]any{
			"pair_token":     sess.PairToken,
			"relay_ws_token": sess.RelayWSToken,
		},
		"timers": map[string]any{
			"grace_window_seconds": sess.GraceWindowSeconds,
			"max_session_seconds":  sess.MaxSessionSeconds,
		},
	}
	return resp
}
```

**Step 5: Update NewRouter caller in cmd/api/main.go**

Find the `NewRouter` call in the main entrypoint and add the dns client:

```go
dnsClient := dns.NewClient()
handler := api.NewRouter(cfg, st, prov, dnsClient)
```

Add import: `"github.com/telemyapp/aegis-control-plane/internal/dns"`

**Step 6: Verify build**

```bash
cd E:/Code/telemyapp/telemy-v0.0.4/aegis-control-plane && go build ./...
```

Expected: Clean build.

**Step 7: Commit**

```bash
git add aegis-control-plane/internal/api/router.go aegis-control-plane/internal/api/handlers.go aegis-control-plane/cmd/api/main.go
git commit -m "feat(api): wire DNS create/delete into relay start/stop handlers"
```

---

### Task 5: Model Changes - Add RelayHostname to Session

**Files:**
- Modify: `aegis-control-plane/internal/model/types.go`
- Modify: `aegis-control-plane/internal/store/store.go` (update queries to join relay_slug)

**Step 1: Add RelayHostname field to Session struct**

In `model/types.go`, add after `WSURL`:

```go
type Session struct {
	ID                 string
	UserID             string
	RelayInstanceID    *string
	RelayAWSInstanceID string
	Status             SessionStatus
	Region             string
	PairToken          string
	RelayWSToken       string
	PublicIP           string
	SRTPort            int
	WSURL              string
	RelayHostname      string        // e.g. "k7mx2p.relay.telemyapp.com"
	StartedAt          time.Time
	StoppedAt          *time.Time
	DurationSeconds    int
	GraceWindowSeconds int
	MaxSessionSeconds  int
}
```

**Step 2: Update GetActiveSession query to join relay_slug**

In `store.go`, update the `GetActiveSession` query (line 81-89) to also select the relay hostname:

```go
func (s *Store) GetActiveSession(ctx context.Context, userID string) (*model.Session, error) {
	const q = `
select s.id, s.user_id, coalesce(s.relay_instance_id, ''), coalesce(ri.aws_instance_id, ''), s.status, s.region, s.pair_token, s.relay_ws_token,
       coalesce(host(ri.public_ip), ''), coalesce(ri.srt_port, 9000), coalesce(ri.ws_url, ''),
       s.started_at, s.stopped_at, s.duration_seconds, s.grace_window_seconds, s.max_session_seconds,
       coalesce(u.relay_slug, '')
from sessions s
left join relay_instances ri on ri.id = s.relay_instance_id
left join users u on u.id = s.user_id
where user_id = $1 and status in ('provisioning', 'active', 'grace')
order by s.created_at desc
limit 1`

	var out model.Session
	var relayInstanceID string
	var stoppedAt *time.Time
	var relaySlug string
	if err := s.db.QueryRow(ctx, q, userID).Scan(
		&out.ID, &out.UserID, &relayInstanceID, &out.RelayAWSInstanceID, &out.Status, &out.Region, &out.PairToken, &out.RelayWSToken,
		&out.PublicIP, &out.SRTPort, &out.WSURL,
		&out.StartedAt, &stoppedAt, &out.DurationSeconds, &out.GraceWindowSeconds, &out.MaxSessionSeconds,
		&relaySlug,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, nil
		}
		return nil, err
	}
	out.StoppedAt = stoppedAt
	out.RelayInstanceID = strPtr(relayInstanceID)
	if relaySlug != "" && out.PublicIP != "" {
		out.RelayHostname = relaySlug + ".relay.telemyapp.com"
	}
	return &out, nil
}
```

Apply the same pattern to `getActiveSessionTx` and `getSessionByIDTx` — add `coalesce(u.relay_slug, '')` to SELECT, add `left join users u on u.id = s.user_id` to FROM, add `&relaySlug` to Scan, and set `RelayHostname` if both slug and public_ip are present.

**Step 3: Verify build**

```bash
cd E:/Code/telemyapp/telemy-v0.0.4/aegis-control-plane && go build ./...
```

**Step 4: Commit**

```bash
git add aegis-control-plane/internal/model/types.go aegis-control-plane/internal/store/store.go
git commit -m "feat(model): add RelayHostname to Session, join relay_slug in queries"
```

---

### Task 6: Deploy Control Plane

**Files:**
- No code changes, operational deployment

**Step 1: Cross-compile**

```bash
cd E:/Code/telemyapp/telemy-v0.0.4/aegis-control-plane && GOOS=linux GOARCH=amd64 go build -o /tmp/aegis-api-linux ./cmd/api/
```

**Step 2: Upload and restart**

```bash
scp -i ~/.ssh/id_server_new /tmp/aegis-api-linux ec2-user@52.13.2.122:/tmp/aegis-api
ssh -i ~/.ssh/id_server_new ec2-user@52.13.2.122 "sudo systemctl stop aegis-api && sudo cp /tmp/aegis-api /opt/aegis/bin/aegis-api && sudo systemctl start aegis-api"
```

**Step 3: Add env vars if not present**

```bash
ssh -i ~/.ssh/id_server_new ec2-user@52.13.2.122 "grep CLOUDFLARE /etc/aegis-control-plane.env"
```

If missing, add:
```bash
ssh -i ~/.ssh/id_server_new ec2-user@52.13.2.122 "echo -e 'CLOUDFLARE_DNS_TOKEN=5wcLbiS_-_ZJwx_WLeJZfyoO3ucuUstaKWCCJ9aU\nCLOUDFLARE_ZONE_ID=69113d75f6631d1eab3b7b9f97e92ed9' | sudo tee -a /etc/aegis-control-plane.env"
```

**Step 4: Verify healthz**

```bash
curl https://api.telemyapp.com/healthz
```

Expected: `{"status":"ok"}`

---

### Task 7: C++ Plugin - Parse relay_hostname

**Files:**
- Modify: `obs-plugin/src/relay_client.h` (add field to RelaySession struct)
- Modify: `obs-plugin/src/relay_client.cpp` (parse relay_hostname from JSON)
- Modify: `obs-plugin/src/metrics_collector.cpp` (emit relay_hostname in snapshot)
- Modify: `obs-plugin/src/metrics_collector.h` (if needed for signature change)

**Step 1: Add relay_hostname to RelaySession struct**

In `relay_client.h`, add after `ws_url` (line 18):

```cpp
struct RelaySession {
    std::string session_id;
    std::string status;
    std::string region;
    std::string public_ip;
    int         srt_port = 9000;
    std::string ws_url;
    std::string relay_hostname;  // e.g. "k7mx2p.relay.telemyapp.com"
    std::string pair_token;
    int         grace_window_seconds = 0;
    int         max_session_seconds = 0;
};
```

**Step 2: Parse relay_hostname in ParseSessionResponse**

In `relay_client.cpp`, after `session.ws_url` line (around line 79), add:

```cpp
    session.relay_hostname = relayObj["relay_hostname"].toString().toStdString();
```

**Step 3: Emit relay_hostname in BuildStatusSnapshotJson**

In `metrics_collector.cpp`, in the relay connection info block (around line 465-468), add after `relay_srt_port`:

```cpp
    if (relay_session) {
        os << "\"relay_public_ip\":\"" << JsonEscape(relay_session->public_ip) << "\",";
        os << "\"relay_srt_port\":" << relay_session->srt_port << ",";
        if (!relay_session->relay_hostname.empty()) {
            os << "\"relay_hostname\":\"" << JsonEscape(relay_session->relay_hostname) << "\",";
        }
    }
```

**Step 4: Commit** (no build verification without OBS SDK setup)

```bash
git add obs-plugin/src/relay_client.h obs-plugin/src/relay_client.cpp obs-plugin/src/metrics_collector.cpp
git commit -m "feat(plugin): parse and emit relay_hostname in telemetry snapshot"
```

---

### Task 8: Dock UI - Display Relay Hostname as Ingest URL

**Files:**
- Modify: `obs-plugin/dock/aegis-dock-bridge.js` (pass relay_hostname through)
- Modify: `obs-plugin/dock/aegis-dock.jsx` (prefer hostname over IP in ingest URL)
- Modify: `obs-plugin/dock/use-simulated-state.js` (add relay_hostname to sim data)

**Step 1: Pass relay_hostname through the bridge**

In `aegis-dock-bridge.js`, in the relay data mapping (around line 325), add `relayHostname`:

```js
          publicIp: relay.public_ip || snap.relay_public_ip || null,
          srtPort: relay.srt_port || snap.relay_srt_port || 5000,
          relayHostname: relay.relay_hostname || snap.relay_hostname || null,
          ingestUrl: (relay.relay_hostname || relay.public_ip || snap.relay_public_ip)
            ? ("srtla://" + (relay.relay_hostname || relay.public_ip || snap.relay_public_ip) + ":" + String(relay.srt_port || snap.relay_srt_port || 5000))
            : null,
```

**Step 2: Update aegis-dock.jsx ingest URL derivation**

In `aegis-dock.jsx`, update the `relayIngestUrl` line (around line 469):

```jsx
  const relayIngestUrl = (relay.relayHostname || relay.publicIp)
    ? "srtla://" + (relay.relayHostname || relay.publicIp) + ":" + (relay.srtPort || 5000)
    : null;
```

**Step 3: Update simulated state**

In `use-simulated-state.js`, add `relayHostname` to the simulated relay data (line 12 area):

```js
    relayHostname: "k7mx2p.relay.telemyapp.com",
    ingestUrl: "srtla://k7mx2p.relay.telemyapp.com:5000",
```

**Step 4: Rebuild dock bundle**

```bash
cd E:/Code/telemyapp/telemy-v0.0.4/obs-plugin/dock && NODE_PATH=../../../dock-preview/node_modules npx esbuild aegis-dock-entry.jsx --bundle --format=iife --jsx=automatic --outfile=aegis-dock-app.js --target=es2020 --minify
```

**Step 5: Commit**

```bash
git add obs-plugin/dock/aegis-dock-bridge.js obs-plugin/dock/aegis-dock.jsx obs-plugin/dock/use-simulated-state.js obs-plugin/dock/aegis-dock-app.js
git commit -m "feat(dock): show relay hostname as ingest URL, fall back to raw IP"
```

---

### Task 9: E2E Validation

**Step 1: Restart OBS** (to load new dock bundle)

**Step 2: Activate relay from OBS dock**

**Step 3: Verify in Cloudflare**
- Check Cloudflare DNS dashboard for new A record under `relay.telemyapp.com`
- The record should point to the relay EC2 IP

**Step 4: Verify in dock**
- Ingest URL should show `srtla://{slug}.relay.telemyapp.com:5000` instead of raw IP
- Copy button should copy the hostname-based URL

**Step 5: Deactivate relay**
- DNS record should be deleted from Cloudflare

**Step 6: Verify DNS cleanup**
- Confirm A record is removed from Cloudflare dashboard

---

### Task 10: Final Commit and Push

**Step 1: Review all changes**

```bash
cd E:/Code/telemyapp/telemy-v0.0.4 && git log --oneline -10
git diff --stat HEAD~8
```

**Step 2: Push to GitHub and GitLab**

```bash
git push github main && git push origin main
```
