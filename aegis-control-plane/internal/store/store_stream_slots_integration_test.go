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

const userStreamSlotsTestDDL = `
CREATE TABLE user_stream_slots (
	user_id      TEXT NOT NULL,
	slot_number  INTEGER NOT NULL,
	label        TEXT,
	stream_token TEXT NOT NULL,
	PRIMARY KEY (user_id, slot_number),
	UNIQUE (stream_token)
);
`

func TestStore_ListUserStreamSlots(t *testing.T) {
	ctx := context.Background()
	st, db, cleanup := newUserStreamSlotsIntegrationStore(t)
	defer cleanup()

	seedUserStreamSlot(t, db, "user-1", 2, "Backup", "slot-b")
	seedUserStreamSlot(t, db, "user-1", 1, "Primary", "slot-a")
	seedUserStreamSlot(t, db, "user-2", 1, "Other", "slot-z")

	slots, err := st.ListUserStreamSlots(ctx, "user-1")
	if err != nil {
		t.Fatalf("ListUserStreamSlots returned error: %v", err)
	}
	if len(slots) != 2 {
		t.Fatalf("ListUserStreamSlots returned %d slots, want 2", len(slots))
	}
	if slots[0].SlotNumber != 1 || slots[0].Label != "Primary" || slots[0].StreamToken != "slot-a" {
		t.Fatalf("unexpected first slot: %+v", slots[0])
	}
	if slots[1].SlotNumber != 2 || slots[1].Label != "Backup" || slots[1].StreamToken != "slot-b" {
		t.Fatalf("unexpected second slot: %+v", slots[1])
	}
}

func TestStore_ListUserStreamSlots_NotFound(t *testing.T) {
	ctx := context.Background()
	st, _, cleanup := newUserStreamSlotsIntegrationStore(t)
	defer cleanup()

	if _, err := st.ListUserStreamSlots(ctx, "missing-user"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("ListUserStreamSlots err = %v, want ErrNotFound", err)
	}
}

func TestStore_GetUserStreamSlotByToken(t *testing.T) {
	ctx := context.Background()
	st, db, cleanup := newUserStreamSlotsIntegrationStore(t)
	defer cleanup()

	seedUserStreamSlot(t, db, "user-1", 3, "", "slot-c")

	slot, err := st.GetUserStreamSlotByToken(ctx, "user-1", "slot-c")
	if err != nil {
		t.Fatalf("GetUserStreamSlotByToken returned error: %v", err)
	}
	if slot.SlotNumber != 3 || slot.Label != "" || slot.StreamToken != "slot-c" {
		t.Fatalf("unexpected slot: %+v", slot)
	}
}

func TestStore_GetUserStreamSlotByToken_NotFound(t *testing.T) {
	ctx := context.Background()
	st, db, cleanup := newUserStreamSlotsIntegrationStore(t)
	defer cleanup()

	seedUserStreamSlot(t, db, "user-1", 1, "Primary", "slot-a")

	if _, err := st.GetUserStreamSlotByToken(ctx, "user-1", "missing"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("GetUserStreamSlotByToken err = %v, want ErrNotFound", err)
	}
}

func newUserStreamSlotsIntegrationStore(t *testing.T) (*Store, *pgxpool.Pool, func()) {
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

	schemaName := fmt.Sprintf("user_stream_slots_it_%d", time.Now().UnixNano())
	if _, err := adminPool.Exec(ctx, "CREATE SCHEMA "+schemaName); err != nil {
		adminPool.Close()
		t.Fatalf("create schema %s: %v", schemaName, err)
	}

	cfg, err := pgxpool.ParseConfig(databaseURL)
	if err != nil {
		dropUserStreamSlotsSchema(t, ctx, adminPool, schemaName)
		adminPool.Close()
		t.Fatalf("parse database config: %v", err)
	}
	if cfg.ConnConfig.RuntimeParams == nil {
		cfg.ConnConfig.RuntimeParams = map[string]string{}
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schemaName

	testPool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		dropUserStreamSlotsSchema(t, ctx, adminPool, schemaName)
		adminPool.Close()
		t.Fatalf("connect test pool: %v", err)
	}

	if _, err := testPool.Exec(ctx, userStreamSlotsTestDDL); err != nil {
		testPool.Close()
		dropUserStreamSlotsSchema(t, ctx, adminPool, schemaName)
		adminPool.Close()
		t.Fatalf("apply user_stream_slots schema: %v", err)
	}

	cleanup := func() {
		testPool.Close()
		dropUserStreamSlotsSchema(t, ctx, adminPool, schemaName)
		adminPool.Close()
	}

	return New(testPool), testPool, cleanup
}

func dropUserStreamSlotsSchema(t *testing.T, ctx context.Context, adminPool *pgxpool.Pool, schemaName string) {
	t.Helper()
	if _, err := adminPool.Exec(ctx, "DROP SCHEMA "+schemaName+" CASCADE"); err != nil {
		t.Fatalf("drop schema %s: %v", schemaName, err)
	}
}

func seedUserStreamSlot(t *testing.T, db *pgxpool.Pool, userID string, slotNumber int, label, streamToken string) {
	t.Helper()
	const q = `
INSERT INTO user_stream_slots (user_id, slot_number, label, stream_token)
VALUES ($1, $2, nullif($3, ''), $4)`
	if _, err := db.Exec(context.Background(), q, userID, slotNumber, label, streamToken); err != nil {
		t.Fatalf("seed user_stream_slot user=%s slot=%d: %v", userID, slotNumber, err)
	}
}
