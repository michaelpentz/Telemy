package api

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/telemyapp/aegis-control-plane/internal/model"
)

func TestUserStreamSlots_ReturnsSlots(t *testing.T) {
	now := time.Now().UTC()
	ms := &mockStore{
		getAuthSessionFn: func(_ context.Context, sessionID string) (*model.AuthSession, error) {
			return &model.AuthSession{ID: sessionID, UserID: "usr_1", ExpiresAt: now.Add(30 * time.Minute)}, nil
		},
		listUserStreamSlotsFn: func(_ context.Context, userID string) ([]model.UserStreamSlot, error) {
			if userID != "usr_1" {
				t.Fatalf("unexpected userID: %s", userID)
			}
			return []model.UserStreamSlot{
				{SlotNumber: 1, Label: "Primary", StreamToken: "slot-a"},
				{SlotNumber: 2, Label: "Backup", StreamToken: "slot-b"},
			}, nil
		},
	}

	_, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
	req := httptest.NewRequest(http.MethodGet, "/api/v1/user/stream-slots", nil)
	req.Header.Set("Authorization", "Bearer "+testSessionJWT(t, "test-secret", "usr_1", "aut_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
	}

	var resp struct {
		StreamSlots []model.UserStreamSlot `json:"stream_slots"`
	}
	if err := json.NewDecoder(rr.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if len(resp.StreamSlots) != 2 {
		t.Fatalf("unexpected stream_slots length: %d", len(resp.StreamSlots))
	}
	if resp.StreamSlots[0].SlotNumber != 1 || resp.StreamSlots[0].Label != "Primary" || resp.StreamSlots[0].StreamToken != "slot-a" {
		t.Fatalf("unexpected first slot: %+v", resp.StreamSlots[0])
	}
}

func TestUserStreamSlots_ReturnsEmptyListWhenNoSlots(t *testing.T) {
	now := time.Now().UTC()
	ms := &mockStore{
		getAuthSessionFn: func(_ context.Context, sessionID string) (*model.AuthSession, error) {
			return &model.AuthSession{ID: sessionID, UserID: "usr_1", ExpiresAt: now.Add(30 * time.Minute)}, nil
		},
	}

	_, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
	req := httptest.NewRequest(http.MethodGet, "/api/v1/user/stream-slots", nil)
	req.Header.Set("Authorization", "Bearer "+testSessionJWT(t, "test-secret", "usr_1", "aut_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
	}

	var resp struct {
		StreamSlots []model.UserStreamSlot `json:"stream_slots"`
	}
	if err := json.NewDecoder(rr.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if len(resp.StreamSlots) != 0 {
		t.Fatalf("expected empty stream_slots, got %+v", resp.StreamSlots)
	}
}
