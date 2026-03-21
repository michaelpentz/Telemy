package store

import (
	"context"
	"errors"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

const relayPoolTestDDL = `
CREATE TABLE relay_pool (
	server_id         TEXT PRIMARY KEY,
	provider          TEXT NOT NULL,
	host              TEXT NOT NULL,
	ip                INET NOT NULL,
	region            TEXT NOT NULL,
	status            TEXT NOT NULL DEFAULT 'active',
	current_sessions  INTEGER NOT NULL DEFAULT 0,
	max_sessions      INTEGER NOT NULL DEFAULT 10,
	created_at        TIMESTAMPTZ DEFAULT NOW(),
	last_health_check TIMESTAMPTZ,
	health_status     TEXT DEFAULT 'unknown'
);

CREATE TABLE relay_assignments (
	id            SERIAL PRIMARY KEY,
	user_id       TEXT NOT NULL,
	session_id    TEXT NOT NULL,
	connection_id TEXT,
	server_id     TEXT NOT NULL REFERENCES relay_pool(server_id),
	stream_token  TEXT NOT NULL,
	assigned_at   TIMESTAMPTZ DEFAULT NOW(),
	released_at   TIMESTAMPTZ,
	UNIQUE(session_id)
);

CREATE INDEX relay_assignments_session_idx ON relay_assignments(session_id);
CREATE INDEX relay_pool_region_status_idx ON relay_pool(region, status) WHERE status = 'active';
`

func TestRelayPoolStore_AssignGetAndReleaseLifecycle(t *testing.T) {
	ctx := context.Background()
	st, db, cleanup := newRelayPoolIntegrationStore(t)
	defer cleanup()

	seedRelayServer(t, db, "west-busy", "west-busy.example.com", "198.51.100.11", "us-west-2", "active", "healthy", 3, 5)
	seedRelayServer(t, db, "west-idle", "west-idle.example.com", "198.51.100.12", "us-west-2", "active", "healthy", 1, 5)
	seedRelayServer(t, db, "east-idle", "east-idle.example.com", "198.51.100.13", "us-east-1", "active", "healthy", 0, 5)

	assignment, err := st.AssignRelay(ctx, "user-1", "session-1", "conn-1", "us-west-2", "stream-1")
	if err != nil {
		t.Fatalf("AssignRelay returned error: %v", err)
	}
	if assignment.ServerID != "west-idle" {
		t.Fatalf("AssignRelay chose %q, want west-idle", assignment.ServerID)
	}
	if assignment.Host != "west-idle.example.com" {
		t.Fatalf("AssignRelay host = %q, want west-idle.example.com", assignment.Host)
	}
	if assignment.IP != "198.51.100.12" {
		t.Fatalf("AssignRelay ip = %q, want 198.51.100.12", assignment.IP)
	}

	got, err := st.GetRelayAssignment(ctx, "session-1")
	if err != nil {
		t.Fatalf("GetRelayAssignment returned error: %v", err)
	}
	if got.ConnectionID != "conn-1" {
		t.Fatalf("GetRelayAssignment connection_id = %q, want conn-1", got.ConnectionID)
	}

	if sessions := relayCurrentSessions(t, db, "west-idle"); sessions != 2 {
		t.Fatalf("current_sessions after assign = %d, want 2", sessions)
	}

	if err := st.ReleaseRelay(ctx, "session-1"); err != nil {
		t.Fatalf("ReleaseRelay returned error: %v", err)
	}
	if sessions := relayCurrentSessions(t, db, "west-idle"); sessions != 1 {
		t.Fatalf("current_sessions after release = %d, want 1", sessions)
	}
	if !relayAssignmentReleased(t, db, "session-1") {
		t.Fatal("expected relay assignment to be marked released")
	}

	if _, err := st.GetRelayAssignment(ctx, "session-1"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("GetRelayAssignment after release err = %v, want ErrNotFound", err)
	}

	if err := st.ReleaseRelay(ctx, "session-1"); err != nil {
		t.Fatalf("second ReleaseRelay returned error: %v", err)
	}
	if sessions := relayCurrentSessions(t, db, "west-idle"); sessions != 1 {
		t.Fatalf("current_sessions after second release = %d, want 1", sessions)
	}
}

func TestRelayPoolStore_AssignRelayFallbackAndCapacityError(t *testing.T) {
	ctx := context.Background()
	st, db, cleanup := newRelayPoolIntegrationStore(t)
	defer cleanup()

	seedRelayServer(t, db, "preferred-full", "preferred-full.example.com", "198.51.100.21", "us-west-2", "active", "healthy", 2, 2)
	seedRelayServer(t, db, "preferred-unhealthy", "preferred-unhealthy.example.com", "198.51.100.22", "us-west-2", "active", "unhealthy", 0, 2)
	seedRelayServer(t, db, "fallback", "fallback.example.com", "198.51.100.23", "us-east-1", "active", "healthy", 0, 2)

	assignment, err := st.AssignRelay(ctx, "user-2", "session-2", "", "us-west-2", "stream-2")
	if err != nil {
		t.Fatalf("AssignRelay fallback returned error: %v", err)
	}
	if assignment.ServerID != "fallback" {
		t.Fatalf("AssignRelay fallback chose %q, want fallback", assignment.ServerID)
	}
	if assignment.ConnectionID != "" {
		t.Fatalf("AssignRelay connection_id = %q, want empty string", assignment.ConnectionID)
	}

	if stored := relayAssignmentConnectionID(t, db, "session-2"); stored != "" {
		t.Fatalf("stored connection_id = %q, want empty string", stored)
	}

	if err := st.ReleaseRelay(ctx, "session-2"); err != nil {
		t.Fatalf("ReleaseRelay fallback assignment returned error: %v", err)
	}

	if _, err := st.AssignRelay(ctx, "user-3", "session-3", "", "ap-south-1", "stream-3"); !errors.Is(err, ErrNoRelayCapacity) {
		t.Fatalf("AssignRelay no capacity err = %v, want ErrNoRelayCapacity", err)
	}
}

func TestRelayPoolStore_ListActiveServersAndUpdateHealth(t *testing.T) {
	ctx := context.Background()
	st, db, cleanup := newRelayPoolIntegrationStore(t)
	defer cleanup()

	seedRelayServer(t, db, "srv-active", "active.example.com", "198.51.100.31", "us-west-2", "active", "healthy", 1, 10)
	seedRelayServer(t, db, "srv-draining", "draining.example.com", "198.51.100.32", "us-west-2", "draining", "unknown", 2, 10)
	seedRelayServer(t, db, "srv-inactive", "inactive.example.com", "198.51.100.33", "us-west-2", "inactive", "healthy", 0, 10)

	servers, err := st.ListActiveRelayServers(ctx)
	if err != nil {
		t.Fatalf("ListActiveRelayServers returned error: %v", err)
	}
	if len(servers) != 2 {
		t.Fatalf("ListActiveRelayServers returned %d servers, want 2", len(servers))
	}
	if servers[0].ServerID != "srv-active" || servers[1].ServerID != "srv-draining" {
		t.Fatalf("ListActiveRelayServers order = [%s %s], want [srv-active srv-draining]", servers[0].ServerID, servers[1].ServerID)
	}

	if err := st.UpdateRelayServerHealth(ctx, "srv-draining", "healthy"); err != nil {
		t.Fatalf("UpdateRelayServerHealth returned error: %v", err)
	}
	if health := relayHealthStatus(t, db, "srv-draining"); health != "healthy" {
		t.Fatalf("health_status after update = %q, want healthy", health)
	}
	if !relayHasHealthCheckTimestamp(t, db, "srv-draining") {
		t.Fatal("expected last_health_check to be set")
	}
}

func newRelayPoolIntegrationStore(t *testing.T) (*Store, *pgxpool.Pool, func()) {
	t.Helper()

	databaseURL := os.Getenv("AEGIS_DATABASE_URL")
	if databaseURL == "" {
		t.Skip("AEGIS_DATABASE_URL is not set")
	}

	ctx := context.Background()
	adminPool, err := pgxpool.New(ctx, databaseURL)
	if err != nil {
		t.Fatalf("connect admin pool: %v", err)
	}

	schemaName := fmt.Sprintf("relay_pool_it_%d", time.Now().UnixNano())
	if _, err := adminPool.Exec(ctx, "CREATE SCHEMA "+schemaName); err != nil {
		adminPool.Close()
		t.Fatalf("create schema %s: %v", schemaName, err)
	}

	cfg, err := pgxpool.ParseConfig(databaseURL)
	if err != nil {
		dropRelayPoolSchema(t, ctx, adminPool, schemaName)
		adminPool.Close()
		t.Fatalf("parse database config: %v", err)
	}
	if cfg.ConnConfig.RuntimeParams == nil {
		cfg.ConnConfig.RuntimeParams = map[string]string{}
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schemaName

	testPool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		dropRelayPoolSchema(t, ctx, adminPool, schemaName)
		adminPool.Close()
		t.Fatalf("connect test pool: %v", err)
	}

	if _, err := testPool.Exec(ctx, relayPoolTestDDL); err != nil {
		testPool.Close()
		dropRelayPoolSchema(t, ctx, adminPool, schemaName)
		adminPool.Close()
		t.Fatalf("apply relay pool schema: %v", err)
	}

	cleanup := func() {
		testPool.Close()
		dropRelayPoolSchema(t, ctx, adminPool, schemaName)
		adminPool.Close()
	}

	return New(testPool), testPool, cleanup
}

func dropRelayPoolSchema(t *testing.T, ctx context.Context, adminPool *pgxpool.Pool, schemaName string) {
	t.Helper()
	if _, err := adminPool.Exec(ctx, "DROP SCHEMA "+schemaName+" CASCADE"); err != nil {
		t.Fatalf("drop schema %s: %v", schemaName, err)
	}
}

func seedRelayServer(t *testing.T, db *pgxpool.Pool, serverID, host, ip, region, status, health string, currentSessions, maxSessions int) {
	t.Helper()
	const q = `
INSERT INTO relay_pool (server_id, provider, host, ip, region, status, health_status, current_sessions, max_sessions)
VALUES ($1, 'fake', $2, $3, $4, $5, $6, $7, $8)`
	if _, err := db.Exec(context.Background(), q, serverID, host, ip, region, status, health, currentSessions, maxSessions); err != nil {
		t.Fatalf("seed relay server %s: %v", serverID, err)
	}
}

func relayCurrentSessions(t *testing.T, db *pgxpool.Pool, serverID string) int {
	t.Helper()
	var currentSessions int
	if err := db.QueryRow(context.Background(), "SELECT current_sessions FROM relay_pool WHERE server_id = $1", serverID).Scan(&currentSessions); err != nil {
		t.Fatalf("query current_sessions for %s: %v", serverID, err)
	}
	return currentSessions
}

func relayAssignmentReleased(t *testing.T, db *pgxpool.Pool, sessionID string) bool {
	t.Helper()
	var releasedAt *time.Time
	if err := db.QueryRow(context.Background(), "SELECT released_at FROM relay_assignments WHERE session_id = $1", sessionID).Scan(&releasedAt); err != nil {
		t.Fatalf("query released_at for %s: %v", sessionID, err)
	}
	return releasedAt != nil
}

func relayAssignmentConnectionID(t *testing.T, db *pgxpool.Pool, sessionID string) string {
	t.Helper()
	var connectionID *string
	if err := db.QueryRow(context.Background(), "SELECT connection_id FROM relay_assignments WHERE session_id = $1", sessionID).Scan(&connectionID); err != nil {
		t.Fatalf("query connection_id for %s: %v", sessionID, err)
	}
	if connectionID == nil {
		return ""
	}
	return *connectionID
}

func relayHealthStatus(t *testing.T, db *pgxpool.Pool, serverID string) string {
	t.Helper()
	var health string
	if err := db.QueryRow(context.Background(), "SELECT health_status FROM relay_pool WHERE server_id = $1", serverID).Scan(&health); err != nil {
		t.Fatalf("query health_status for %s: %v", serverID, err)
	}
	return health
}

func relayHasHealthCheckTimestamp(t *testing.T, db *pgxpool.Pool, serverID string) bool {
	t.Helper()
	var lastHealthCheck *time.Time
	if err := db.QueryRow(context.Background(), "SELECT last_health_check FROM relay_pool WHERE server_id = $1", serverID).Scan(&lastHealthCheck); err != nil {
		t.Fatalf("query last_health_check for %s: %v", serverID, err)
	}
	return lastHealthCheck != nil
}
