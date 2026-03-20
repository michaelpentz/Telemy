package dns

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// newTestClient returns a Client wired to the given httptest.Server.
func newTestClient(srv *httptest.Server) *Client {
	return &Client{
		zoneID:     "zone123",
		apiToken:   "tok_test",
		baseDomain: "relay.example.com",
		httpClient: &http.Client{Timeout: 5 * time.Second},
		baseURL:    srv.URL,
	}
}

// cfListResponse builds a Cloudflare-shaped list response with the given record IDs.
func cfListResponse(ids ...string) []byte {
	type rec struct {
		ID string `json:"id"`
	}
	type resp struct {
		Result []rec `json:"result"`
	}
	r := resp{}
	for _, id := range ids {
		r.Result = append(r.Result, rec{ID: id})
	}
	b, _ := json.Marshal(r)
	return b
}

// cfEmptyList is a Cloudflare list response with no records.
var cfEmptyList = cfListResponse()

// cfOK is a minimal successful Cloudflare mutation response.
var cfOK = []byte(`{"result":{"id":"rec999"},"success":true}`)

// ---- Enabled / FQDN ----

func TestEnabled_WhenConfigured(t *testing.T) {
	c := &Client{zoneID: "z", apiToken: "t"}
	if !c.Enabled() {
		t.Fatal("expected Enabled() == true when both fields set")
	}
}

func TestEnabled_WhenMissingZoneID(t *testing.T) {
	c := &Client{apiToken: "t"}
	if c.Enabled() {
		t.Fatal("expected Enabled() == false when zoneID empty")
	}
}

func TestEnabled_WhenMissingToken(t *testing.T) {
	c := &Client{zoneID: "z"}
	if c.Enabled() {
		t.Fatal("expected Enabled() == false when apiToken empty")
	}
}

func TestFQDN(t *testing.T) {
	c := &Client{baseDomain: "relay.example.com"}
	got := c.FQDN("abc123")
	if got != "abc123.relay.example.com" {
		t.Fatalf("unexpected FQDN: %s", got)
	}
}

// ---- CreateOrUpdateRecord ----

func TestCreateOrUpdateRecord_DisabledNoOp(t *testing.T) {
	// Client with no zoneID — should skip without hitting the network.
	c := &Client{baseDomain: "relay.example.com", httpClient: &http.Client{}}
	if err := c.CreateOrUpdateRecord("slug1", "1.2.3.4"); err != nil {
		t.Fatalf("expected nil error, got %v", err)
	}
}

func TestCreateOrUpdateRecord_POSTWhenNoExisting(t *testing.T) {
	var method string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			w.Write(cfEmptyList)
		default:
			method = r.Method
			w.Write(cfOK)
		}
	}))
	defer srv.Close()

	c := newTestClient(srv)
	if err := c.CreateOrUpdateRecord("slug1", "1.2.3.4"); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if method != http.MethodPost {
		t.Fatalf("expected POST for new record, got %s", method)
	}
}

func TestCreateOrUpdateRecord_PUTWhenExisting(t *testing.T) {
	var method string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			w.Write(cfListResponse("existing_rec_id"))
		default:
			method = r.Method
			w.Write(cfOK)
		}
	}))
	defer srv.Close()

	c := newTestClient(srv)
	if err := c.CreateOrUpdateRecord("slug1", "1.2.3.4"); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if method != http.MethodPut {
		t.Fatalf("expected PUT for existing record, got %s", method)
	}
}

func TestCreateOrUpdateRecord_PUTURLContainsRecordID(t *testing.T) {
	var putURL string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			w.Write(cfListResponse("rec_abc"))
		default:
			putURL = r.URL.Path
			w.Write(cfOK)
		}
	}))
	defer srv.Close()

	c := newTestClient(srv)
	if err := c.CreateOrUpdateRecord("slug1", "1.2.3.4"); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !strings.HasSuffix(putURL, "/rec_abc") {
		t.Fatalf("PUT URL should end with record ID, got: %s", putURL)
	}
}

func TestCreateOrUpdateRecord_APIError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			w.Write(cfEmptyList)
		default:
			w.WriteHeader(http.StatusForbidden)
			w.Write([]byte(`{"errors":[{"message":"permission denied"}]}`))
		}
	}))
	defer srv.Close()

	c := newTestClient(srv)
	err := c.CreateOrUpdateRecord("slug1", "1.2.3.4")
	if err == nil {
		t.Fatal("expected error on 403 response")
	}
	if !strings.Contains(err.Error(), "403") {
		t.Fatalf("error should mention status code 403: %v", err)
	}
}

func TestCreateOrUpdateRecord_FindRecordAPIError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusUnauthorized)
		w.Write([]byte(`{"errors":[{"message":"invalid token"}]}`))
	}))
	defer srv.Close()

	c := newTestClient(srv)
	err := c.CreateOrUpdateRecord("slug1", "1.2.3.4")
	if err == nil {
		t.Fatal("expected error when list returns 401")
	}
}

func TestCreateOrUpdateRecord_RateLimit(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			w.Write(cfEmptyList)
		default:
			w.WriteHeader(http.StatusTooManyRequests)
			w.Write([]byte(`{"errors":[{"message":"rate limited"}]}`))
		}
	}))
	defer srv.Close()

	c := newTestClient(srv)
	err := c.CreateOrUpdateRecord("slug1", "1.2.3.4")
	if err == nil {
		t.Fatal("expected error on 429 response")
	}
	if !strings.Contains(err.Error(), "429") {
		t.Fatalf("error should mention status code 429: %v", err)
	}
}

func TestCreateOrUpdateRecord_NetworkError(t *testing.T) {
	// Point at a server that is immediately closed to simulate a network error.
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	srv.Close()

	c := newTestClient(srv)
	err := c.CreateOrUpdateRecord("slug1", "1.2.3.4")
	if err == nil {
		t.Fatal("expected error on network failure")
	}
}

// ---- DeleteRecord ----

func TestDeleteRecord_DisabledNoOp(t *testing.T) {
	c := &Client{baseDomain: "relay.example.com", httpClient: &http.Client{}}
	if err := c.DeleteRecord("slug1"); err != nil {
		t.Fatalf("expected nil error, got %v", err)
	}
}

func TestDeleteRecord_NoExistingRecordIsNoOp(t *testing.T) {
	var deleteCalled bool
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			w.Write(cfEmptyList)
		default:
			deleteCalled = true
			w.Write(cfOK)
		}
	}))
	defer srv.Close()

	c := newTestClient(srv)
	if err := c.DeleteRecord("slug1"); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if deleteCalled {
		t.Fatal("DELETE should not be called when no record exists")
	}
}

func TestDeleteRecord_Success(t *testing.T) {
	var deleteMethod string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			w.Write(cfListResponse("rec_to_delete"))
		default:
			deleteMethod = r.Method
			w.Write(cfOK)
		}
	}))
	defer srv.Close()

	c := newTestClient(srv)
	if err := c.DeleteRecord("slug1"); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if deleteMethod != http.MethodDelete {
		t.Fatalf("expected DELETE request, got %s", deleteMethod)
	}
}

func TestDeleteRecord_DeleteURLContainsRecordID(t *testing.T) {
	var deleteURL string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			w.Write(cfListResponse("rec_xyz"))
		default:
			deleteURL = r.URL.Path
			w.Write(cfOK)
		}
	}))
	defer srv.Close()

	c := newTestClient(srv)
	if err := c.DeleteRecord("slug1"); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !strings.HasSuffix(deleteURL, "/rec_xyz") {
		t.Fatalf("DELETE URL should end with record ID, got: %s", deleteURL)
	}
}

func TestDeleteRecord_APIError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			w.Write(cfListResponse("rec1"))
		default:
			w.WriteHeader(http.StatusInternalServerError)
			w.Write([]byte(`{"errors":[{"message":"server error"}]}`))
		}
	}))
	defer srv.Close()

	c := newTestClient(srv)
	err := c.DeleteRecord("slug1")
	if err == nil {
		t.Fatal("expected error on 500 response")
	}
	if !strings.Contains(err.Error(), "500") {
		t.Fatalf("error should mention status code 500: %v", err)
	}
}

func TestDeleteRecord_NetworkError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	srv.Close()

	c := newTestClient(srv)
	err := c.DeleteRecord("slug1")
	if err == nil {
		t.Fatal("expected error on network failure")
	}
}

// ---- Authorization header ----

func TestAuthorizationHeaderSent(t *testing.T) {
	var gotAuth string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotAuth = r.Header.Get("Authorization")
		w.Write(cfEmptyList)
	}))
	defer srv.Close()

	c := newTestClient(srv)
	// Trigger a GET (findRecord) only — ignore the subsequent POST error.
	_ = c.CreateOrUpdateRecord("slug1", "1.2.3.4")

	if gotAuth != "Bearer tok_test" {
		t.Fatalf("expected Bearer token header, got: %q", gotAuth)
	}
}
