package relay

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func newTestSLSServer(t *testing.T, handler http.HandlerFunc) (*httptest.Server, *SLSClient) {
	t.Helper()
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)
	return srv, NewSLSClient(srv.URL, "test-api-key")
}

func TestListStreamIDs_success(t *testing.T) {
	_, client := newTestSLSServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet || r.URL.Path != "/api/stream-ids" {
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
		}
		if r.Header.Get("Authorization") != "Bearer test-api-key" {
			t.Error("missing or wrong Authorization header")
		}
		resp := slsListResponse{
			Status: "success",
			Data: []StreamID{
				{Publisher: "live_aegis", Player: "play_aegis", Description: "main stream"},
			},
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(resp)
	})

	ids, err := client.ListStreamIDs(context.Background())
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(ids) != 1 {
		t.Fatalf("expected 1 stream ID, got %d", len(ids))
	}
	if ids[0].Publisher != "live_aegis" {
		t.Errorf("expected publisher live_aegis, got %s", ids[0].Publisher)
	}
}

func TestListStreamIDs_unauthorized(t *testing.T) {
	_, client := newTestSLSServer(t, func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
	})

	_, err := client.ListStreamIDs(context.Background())
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	slsErr, ok := err.(*SLSError)
	if !ok {
		t.Fatalf("expected *SLSError, got %T", err)
	}
	if slsErr.Code != http.StatusUnauthorized {
		t.Errorf("expected 401, got %d", slsErr.Code)
	}
}

func TestCreateStreamID_success(t *testing.T) {
	_, client := newTestSLSServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/api/stream-ids" {
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
		}
		if r.Header.Get("Content-Type") != "application/json" {
			t.Error("expected Content-Type application/json")
		}
		var body StreamID
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			t.Errorf("failed to decode body: %v", err)
		}
		if body.Publisher != "live_test" || body.Player != "play_test" {
			t.Errorf("unexpected body: %+v", body)
		}
		w.WriteHeader(http.StatusCreated)
	})

	err := client.CreateStreamID(context.Background(), "live_test", "play_test", "test stream")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestCreateStreamID_serverError(t *testing.T) {
	_, client := newTestSLSServer(t, func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "internal error", http.StatusInternalServerError)
	})

	err := client.CreateStreamID(context.Background(), "live_x", "play_x", "")
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	slsErr, ok := err.(*SLSError)
	if !ok {
		t.Fatalf("expected *SLSError, got %T", err)
	}
	if slsErr.Code != http.StatusInternalServerError {
		t.Errorf("expected 500, got %d", slsErr.Code)
	}
}

func TestDeleteStreamID_success(t *testing.T) {
	_, client := newTestSLSServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodDelete || r.URL.Path != "/api/stream-ids/live_aegis" {
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
		}
		w.WriteHeader(http.StatusNoContent)
	})

	err := client.DeleteStreamID(context.Background(), "live_aegis")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestDeleteStreamID_notFound(t *testing.T) {
	_, client := newTestSLSServer(t, func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "not found", http.StatusNotFound)
	})

	err := client.DeleteStreamID(context.Background(), "live_missing")
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	slsErr, ok := err.(*SLSError)
	if !ok {
		t.Fatalf("expected *SLSError, got %T", err)
	}
	if slsErr.Code != http.StatusNotFound {
		t.Errorf("expected 404, got %d", slsErr.Code)
	}
}
