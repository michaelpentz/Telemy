package api

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"

	"github.com/telemyapp/aegis-control-plane/internal/config"
	"github.com/telemyapp/aegis-control-plane/internal/dns"
	"github.com/telemyapp/aegis-control-plane/internal/metrics"
	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/relay"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

type mockStore struct {
	getSessionByIDFn         func(context.Context, string, string) (*model.Session, error)
	stopSessionFn            func(context.Context, string, string) (*model.Session, error)
	startOrGetSessionFn      func(context.Context, store.StartInput) (*model.Session, bool, error)
	activateSessionFn        func(context.Context, store.ActivateProvisionedSessionInput) (*model.Session, error)
	getActiveSessionFn       func(context.Context, string) (*model.Session, error)
	getUsageCurrentFn        func(context.Context, string) (*model.UsageCurrent, error)
	recordRelayHealthEventFn func(context.Context, store.RelayHealthInput) error
	listRelayManifestFn      func(context.Context) ([]model.RelayManifestEntry, error)
	updateProvisionStepFn    func(context.Context, string, string) error
	getUserRelaySlugFn       func(context.Context, string) (string, error)
	finalActivateSessionFn   func(context.Context, string) error
}

func (m *mockStore) StartOrGetSession(ctx context.Context, in store.StartInput) (*model.Session, bool, error) {
	if m.startOrGetSessionFn != nil {
		return m.startOrGetSessionFn(ctx, in)
	}
	return nil, false, nil
}

func (m *mockStore) ActivateProvisionedSession(ctx context.Context, in store.ActivateProvisionedSessionInput) (*model.Session, error) {
	if m.activateSessionFn != nil {
		return m.activateSessionFn(ctx, in)
	}
	return nil, nil
}

func (m *mockStore) GetActiveSession(ctx context.Context, userID string) (*model.Session, error) {
	if m.getActiveSessionFn != nil {
		return m.getActiveSessionFn(ctx, userID)
	}
	return nil, nil
}

func (m *mockStore) GetSessionByID(ctx context.Context, userID, sessionID string) (*model.Session, error) {
	if m.getSessionByIDFn != nil {
		return m.getSessionByIDFn(ctx, userID, sessionID)
	}
	return nil, store.ErrNotFound
}

func (m *mockStore) StopSession(ctx context.Context, userID, sessionID string) (*model.Session, error) {
	if m.stopSessionFn != nil {
		return m.stopSessionFn(ctx, userID, sessionID)
	}
	return nil, store.ErrNotFound
}

func (m *mockStore) GetUsageCurrent(ctx context.Context, userID string) (*model.UsageCurrent, error) {
	if m.getUsageCurrentFn != nil {
		return m.getUsageCurrentFn(ctx, userID)
	}
	return nil, store.ErrNotFound
}

func (m *mockStore) RecordRelayHealth(ctx context.Context, in store.RelayHealthInput) error {
	if m.recordRelayHealthEventFn != nil {
		return m.recordRelayHealthEventFn(ctx, in)
	}
	return nil
}

func (m *mockStore) ListRelayManifest(ctx context.Context) ([]model.RelayManifestEntry, error) {
	if m.listRelayManifestFn != nil {
		return m.listRelayManifestFn(ctx)
	}
	return nil, nil
}

func (m *mockStore) UpdateProvisionStep(ctx context.Context, sessionID, step string) error {
	if m.updateProvisionStepFn != nil {
		return m.updateProvisionStepFn(ctx, sessionID, step)
	}
	return nil
}

func (m *mockStore) GetUserRelaySlug(ctx context.Context, userID string) (string, error) {
	if m.getUserRelaySlugFn != nil {
		return m.getUserRelaySlugFn(ctx, userID)
	}
	return "", store.ErrNotFound
}

func (m *mockStore) FinalActivateSession(ctx context.Context, sessionID string) error {
	if m.finalActivateSessionFn != nil {
		return m.finalActivateSessionFn(ctx, sessionID)
	}
	return nil
}

func (m *mockStore) GetUserEIP(ctx context.Context, userID string) (string, string, error) {
	return "", "", nil
}

func (m *mockStore) SetUserEIP(ctx context.Context, userID, allocationID, publicIP string) error {
	return nil
}

type mockProvisioner struct {
	provisionFn   func(context.Context, relay.ProvisionRequest) (relay.ProvisionResult, error)
	deprovisionFn func(context.Context, relay.DeprovisionRequest) error
}

func (m *mockProvisioner) Provision(ctx context.Context, req relay.ProvisionRequest) (relay.ProvisionResult, error) {
	if m.provisionFn != nil {
		return m.provisionFn(ctx, req)
	}
	return relay.ProvisionResult{
		AWSInstanceID: "i-default",
		AMIID:         "ami-default",
		InstanceType:  "t4g.small",
		PublicIP:      "203.0.113.10",
		SRTPort:       9000,
	}, nil
}

func (m *mockProvisioner) Deprovision(ctx context.Context, req relay.DeprovisionRequest) error {
	if m.deprovisionFn != nil {
		return m.deprovisionFn(ctx, req)
	}
	return nil
}

func TestRelayStop_IdempotentAlreadyStoppedSkipsDeprovision(t *testing.T) {
	stoppedAt := time.Now().UTC()
	ms := &mockStore{
		getSessionByIDFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			return &model.Session{
				ID:                 "ses_1",
				UserID:             "usr_1",
				Status:             model.SessionStopped,
				RelayAWSInstanceID: "i-abc",
			}, nil
		},
		stopSessionFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			return &model.Session{
				ID:        "ses_1",
				UserID:    "usr_1",
				Status:    model.SessionStopped,
				StoppedAt: &stoppedAt,
			}, nil
		},
	}

	deprovCalls := 0
	mp := &mockProvisioner{
		deprovisionFn: func(_ context.Context, _ relay.DeprovisionRequest) error {
			deprovCalls++
			return nil
		},
	}

	_, router := NewRouter(testConfig(), ms, mp, dns.NewClient("relay.telemyapp.com"))
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/stop", jsonBody(map[string]any{
		"session_id": "ses_1",
		"reason":     "user_requested",
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
	}
	if deprovCalls != 0 {
		t.Fatalf("expected no deprovision call for stopped session, got %d", deprovCalls)
	}
}

func TestRelayStop_ActiveSessionCallsDeprovisionThenStops(t *testing.T) {
	stoppedAt := time.Now().UTC()
	ms := &mockStore{
		getSessionByIDFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			return &model.Session{
				ID:                 "ses_2",
				UserID:             "usr_1",
				Status:             model.SessionActive,
				Region:             "us-east-1",
				RelayAWSInstanceID: "i-xyz",
			}, nil
		},
		stopSessionFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			return &model.Session{
				ID:        "ses_2",
				UserID:    "usr_1",
				Status:    model.SessionStopped,
				StoppedAt: &stoppedAt,
			}, nil
		},
	}

	deprovCalls := 0
	mp := &mockProvisioner{
		deprovisionFn: func(_ context.Context, req relay.DeprovisionRequest) error {
			deprovCalls++
			if req.AWSInstanceID != "i-xyz" {
				t.Fatalf("unexpected instance id: %s", req.AWSInstanceID)
			}
			return nil
		},
	}

	_, router := NewRouter(testConfig(), ms, mp, dns.NewClient("relay.telemyapp.com"))
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/stop", jsonBody(map[string]any{
		"session_id": "ses_2",
		"reason":     "user_requested",
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
	}
	if deprovCalls != 1 {
		t.Fatalf("expected exactly one deprovision call, got %d", deprovCalls)
	}
}

func TestRelayStop_DeprovisionFailureReturns500(t *testing.T) {
	ms := &mockStore{
		getSessionByIDFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			return &model.Session{
				ID:                 "ses_3",
				UserID:             "usr_1",
				Status:             model.SessionActive,
				Region:             "us-east-1",
				RelayAWSInstanceID: "i-fail",
			}, nil
		},
		stopSessionFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			t.Fatal("stop should not be called when deprovision fails")
			return nil, nil
		},
	}
	mp := &mockProvisioner{
		deprovisionFn: func(_ context.Context, _ relay.DeprovisionRequest) error {
			return context.DeadlineExceeded
		},
	}

	_, router := NewRouter(testConfig(), ms, mp, dns.NewClient("relay.telemyapp.com"))
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/stop", jsonBody(map[string]any{
		"session_id": "ses_3",
		"reason":     "user_requested",
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusInternalServerError {
		t.Fatalf("expected 500, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestRelayStart_IdempotencyReplaySkipsProvisioning(t *testing.T) {
	idem := "8a849d0e-04eb-4a11-bf8a-6b8e5ea1572f"
	firstSession := &model.Session{
		ID:                 "ses_new",
		UserID:             "usr_1",
		Status:             model.SessionProvisioning,
		Region:             "us-east-1",
		SRTPort:            9000,
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}
	replayedSession := &model.Session{
		ID:                 "ses_new",
		UserID:             "usr_1",
		Status:             model.SessionActive,
		Region:             "us-east-1",
		RelayAWSInstanceID: "i-123",
		PublicIP:           "198.51.100.21",
		SRTPort:            9000,
		PairToken:          "PAIR1234",
		RelayWSToken:       "ws_token",
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}

	// startCalls tracks that store.StartOrGetSession is called each time.
	// On call 1: created=true  → handler spawns async goroutine, returns 201.
	// On call 2: created=false → handler does NOT spawn a second goroutine, returns 200
	//            with the current session state (active after provisioning completes).
	startCalls := 0
	ms := &mockStore{
		startOrGetSessionFn: func(_ context.Context, in store.StartInput) (*model.Session, bool, error) {
			startCalls++
			if in.IdempotencyKey.String() != idem {
				t.Fatalf("unexpected idempotency key: %s", in.IdempotencyKey.String())
			}
			if startCalls == 1 {
				return firstSession, true, nil
			}
			// Simulate live lookup returning active state after provisioning completed.
			return replayedSession, false, nil
		},
	}
	mp := &mockProvisioner{}

	_, router := NewRouter(testConfig(), ms, mp, nil)
	body := map[string]any{
		"region_preference": "us-east-1",
		"client_context":    map[string]any{"requested_by": "dashboard"},
	}
	for i := 0; i < 2; i++ {
		req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/start", jsonBody(body))
		req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
		req.Header.Set("Idempotency-Key", idem)
		rr := httptest.NewRecorder()
		router.ServeHTTP(rr, req)

		if i == 0 && rr.Code != http.StatusCreated {
			t.Fatalf("first start expected 201, got %d body=%s", rr.Code, rr.Body.String())
		}
		if i == 1 && rr.Code != http.StatusOK {
			t.Fatalf("replay start expected 200 with active session, got %d body=%s", rr.Code, rr.Body.String())
		}
		if i == 1 {
			var resp struct {
				Session struct {
					Status string `json:"status"`
				} `json:"session"`
			}
			if err := json.NewDecoder(rr.Body).Decode(&resp); err != nil {
				t.Fatalf("decode replay response: %v", err)
			}
			if resp.Session.Status != "active" {
				t.Fatalf("replay should return active session, got status=%s", resp.Session.Status)
			}
		}
	}

	if startCalls != 2 {
		t.Fatalf("expected 2 start/get calls, got %d", startCalls)
	}
}

func TestRelayStart_DuplicateActiveSessionPreventsProvisioning(t *testing.T) {
	activeSession := &model.Session{
		ID:                 "ses_existing",
		UserID:             "usr_1",
		Status:             model.SessionActive,
		Region:             "eu-west-1",
		RelayAWSInstanceID: "i-existing",
		PublicIP:           "203.0.113.77",
		SRTPort:            9000,
		PairToken:          "EXIST123",
		RelayWSToken:       "existing_ws_token",
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}

	activateCalls := 0
	ms := &mockStore{
		startOrGetSessionFn: func(_ context.Context, _ store.StartInput) (*model.Session, bool, error) {
			return activeSession, false, nil
		},
		activateSessionFn: func(_ context.Context, _ store.ActivateProvisionedSessionInput) (*model.Session, error) {
			activateCalls++
			return nil, nil
		},
	}

	provisionCalls := 0
	mp := &mockProvisioner{
		provisionFn: func(_ context.Context, _ relay.ProvisionRequest) (relay.ProvisionResult, error) {
			provisionCalls++
			return relay.ProvisionResult{}, nil
		},
	}

	_, router := NewRouter(testConfig(), ms, mp, dns.NewClient("relay.telemyapp.com"))
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/start", jsonBody(map[string]any{
		"region_preference": "eu-west-1",
		"client_context": map[string]any{
			"requested_by": "dashboard",
		},
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	req.Header.Set("Idempotency-Key", "e699cf53-cdf9-44c6-835c-f867a8b6aa95")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200 for existing active session, got %d body=%s", rr.Code, rr.Body.String())
	}
	if provisionCalls != 0 {
		t.Fatalf("expected no provisioning for duplicate active session, got %d", provisionCalls)
	}
	if activateCalls != 0 {
		t.Fatalf("expected no activation for duplicate active session, got %d", activateCalls)
	}
}

func TestRelayStart_ProvisionFailureCompensatesByStoppingSession(t *testing.T) {
	// Handler returns 201 immediately and spawns an async pipeline goroutine.
	// Compensation (stop without deprovision) is verified in TestProvisionPipeline_ProvisionFailureCompensates.
	createdSession := &model.Session{
		ID:                 "ses_prov_fail",
		UserID:             "usr_1",
		Status:             model.SessionProvisioning,
		Region:             "us-east-1",
		SRTPort:            9000,
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}
	ms := &mockStore{
		startOrGetSessionFn: func(_ context.Context, _ store.StartInput) (*model.Session, bool, error) {
			return createdSession, true, nil
		},
	}
	mp := &mockProvisioner{}

	_, router := NewRouter(testConfig(), ms, mp, dns.NewClient("relay.telemyapp.com"))
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/start", jsonBody(map[string]any{
		"region_preference": "us-east-1",
		"client_context":    map[string]any{"requested_by": "dashboard"},
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	req.Header.Set("Idempotency-Key", "b9e2bdb0-0ef2-46ba-8201-76558a3d5337")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusCreated {
		t.Fatalf("expected 201 for new provisioning session, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestRelayStart_ActivationFailureCompensatesByDeprovisionAndStoppingSession(t *testing.T) {
	// Handler returns 201 immediately and spawns an async pipeline goroutine.
	// Deprovision+stop compensation is verified in TestProvisionPipeline_ActivationFailureDeprovisions.
	createdSession := &model.Session{
		ID:                 "ses_activate_fail",
		UserID:             "usr_1",
		Status:             model.SessionProvisioning,
		Region:             "us-east-1",
		SRTPort:            9000,
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}
	ms := &mockStore{
		startOrGetSessionFn: func(_ context.Context, _ store.StartInput) (*model.Session, bool, error) {
			return createdSession, true, nil
		},
	}
	mp := &mockProvisioner{}

	_, router := NewRouter(testConfig(), ms, mp, dns.NewClient("relay.telemyapp.com"))
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/start", jsonBody(map[string]any{
		"region_preference": "us-east-1",
		"client_context":    map[string]any{"requested_by": "dashboard"},
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	req.Header.Set("Idempotency-Key", "d4717a10-f714-4ea7-8ee4-df2de023c868")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusCreated {
		t.Fatalf("expected 201 for new provisioning session, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestRelayManifest_ReturnsConfiguredEntries(t *testing.T) {
	ts := time.Date(2026, 2, 21, 18, 0, 0, 0, time.UTC)
	ms := &mockStore{
		listRelayManifestFn: func(_ context.Context) ([]model.RelayManifestEntry, error) {
			return []model.RelayManifestEntry{
				{
					Region:              "eu-west-1",
					AMIID:               "ami-0456efgh",
					DefaultInstanceType: "t4g.small",
					UpdatedAt:           ts,
				},
				{
					Region:              "us-east-1",
					AMIID:               "ami-0123abcd",
					DefaultInstanceType: "t4g.small",
					UpdatedAt:           ts,
				},
			}, nil
		},
	}

	_, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
	req := httptest.NewRequest(http.MethodGet, "/api/v1/relay/manifest", nil)
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
	}
	var body struct {
		Regions []map[string]any `json:"regions"`
	}
	if err := json.Unmarshal(rr.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode body: %v", err)
	}
	if len(body.Regions) != 2 {
		t.Fatalf("expected 2 regions, got %d", len(body.Regions))
	}
}

func TestRelayManifest_EmptyManifestReturns503(t *testing.T) {
	ms := &mockStore{
		listRelayManifestFn: func(_ context.Context) ([]model.RelayManifestEntry, error) {
			return []model.RelayManifestEntry{}, nil
		},
	}

	_, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
	req := httptest.NewRequest(http.MethodGet, "/api/v1/relay/manifest", nil)
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusServiceUnavailable {
		t.Fatalf("expected 503, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestRelayHealth_RejectedPayloadReturns400(t *testing.T) {
	ms := &mockStore{
		recordRelayHealthEventFn: func(_ context.Context, _ store.RelayHealthInput) error {
			return store.ErrRelayHealthRejected
		},
	}

	_, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/health", jsonBody(map[string]any{
		"session_id":             "ses_1",
		"instance_id":            "i-1",
		"ingest_active":          true,
		"egress_active":          true,
		"session_uptime_seconds": 12,
	}))
	req.Header.Set("X-Relay-Auth", "relay-key")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestRelayHealth_StoreFailureReturns500(t *testing.T) {
	ms := &mockStore{
		recordRelayHealthEventFn: func(_ context.Context, _ store.RelayHealthInput) error {
			return errors.New("db unavailable")
		},
	}

	_, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/health", jsonBody(map[string]any{
		"session_id":             "ses_1",
		"instance_id":            "i-1",
		"ingest_active":          true,
		"egress_active":          true,
		"session_uptime_seconds": 12,
	}))
	req.Header.Set("X-Relay-Auth", "relay-key")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusInternalServerError {
		t.Fatalf("expected 500, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestMetricsEndpoint_ExposesPrometheusPayload(t *testing.T) {
	metrics.ResetDefaultForTest()

	ms := &mockStore{}
	_, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
	req := httptest.NewRequest(http.MethodGet, "/metrics", nil)
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
	}
	body := rr.Body.String()
	if body == "" {
		t.Fatal("expected non-empty metrics body")
	}
	if !bytes.Contains(rr.Body.Bytes(), []byte("# TYPE aegis_job_runs_total counter")) {
		t.Fatalf("expected job counter type in metrics payload, body=%s", body)
	}
	if !bytes.Contains(rr.Body.Bytes(), []byte("# TYPE aegis_relay_provision_latency_ms histogram")) {
		t.Fatalf("expected provision histogram type in metrics payload, body=%s", body)
	}
}

// newTestServer creates a Server with no-op probe and dwell so pipeline tests run instantly.
func newTestServer(ms *mockStore, mp *mockProvisioner) *Server {
	return &Server{
		cfg:         testConfig(),
		store:       ms,
		provisioner: mp,
		probeReady:  func(_ context.Context, _ string) bool { return true },
		dwell:       func(_ context.Context, _ time.Duration) {},
		appCtx:      context.Background(),
	}
}

func TestProvisionPipeline_ProvisionFailureCompensates(t *testing.T) {
	stopCalls := 0
	ms := &mockStore{
		stopSessionFn: func(_ context.Context, userID, sessionID string) (*model.Session, error) {
			stopCalls++
			if userID != "usr_1" || sessionID != "ses_prov_fail" {
				t.Errorf("unexpected stop target user=%s session=%s", userID, sessionID)
			}
			return &model.Session{ID: sessionID, UserID: userID, Status: model.SessionStopped}, nil
		},
	}
	deprovCalls := 0
	mp := &mockProvisioner{
		provisionFn: func(_ context.Context, _ relay.ProvisionRequest) (relay.ProvisionResult, error) {
			return relay.ProvisionResult{}, context.DeadlineExceeded
		},
		deprovisionFn: func(_ context.Context, _ relay.DeprovisionRequest) error {
			deprovCalls++
			return nil
		},
	}

	s := newTestServer(ms, mp)
	s.runProvisionPipeline("ses_prov_fail", "usr_1", "us-east-1")

	if stopCalls != 1 {
		t.Fatalf("expected 1 stop compensation call, got %d", stopCalls)
	}
	if deprovCalls != 0 {
		t.Fatalf("expected no deprovision when no instance was launched, got %d", deprovCalls)
	}
}

func TestProvisionPipeline_ActivationFailureDeprovisions(t *testing.T) {
	activateCalls, deprovCalls, stopCalls := 0, 0, 0
	ms := &mockStore{
		activateSessionFn: func(_ context.Context, in store.ActivateProvisionedSessionInput) (*model.Session, error) {
			activateCalls++
			if in.SessionID != "ses_act_fail" {
				t.Errorf("unexpected session id: %s", in.SessionID)
			}
			return nil, context.Canceled
		},
		stopSessionFn: func(_ context.Context, userID, sessionID string) (*model.Session, error) {
			stopCalls++
			return &model.Session{ID: sessionID, UserID: userID, Status: model.SessionStopped}, nil
		},
	}
	mp := &mockProvisioner{
		provisionFn: func(_ context.Context, _ relay.ProvisionRequest) (relay.ProvisionResult, error) {
			return relay.ProvisionResult{
				AWSInstanceID: "i-orphan-risk",
				AMIID:         "ami-123",
				InstanceType:  "t4g.small",
				PublicIP:      "198.51.100.50",
				SRTPort:       5000,
			}, nil
		},
		deprovisionFn: func(_ context.Context, req relay.DeprovisionRequest) error {
			deprovCalls++
			if req.AWSInstanceID != "i-orphan-risk" {
				t.Errorf("unexpected instance id: %s", req.AWSInstanceID)
			}
			return nil
		},
	}

	s := newTestServer(ms, mp)
	s.runProvisionPipeline("ses_act_fail", "usr_1", "us-east-1")

	if activateCalls != 1 {
		t.Fatalf("expected 1 activation call, got %d", activateCalls)
	}
	if deprovCalls != 1 {
		t.Fatalf("expected 1 deprovision compensation call, got %d", deprovCalls)
	}
	if stopCalls != 1 {
		t.Fatalf("expected 1 stop compensation call, got %d", stopCalls)
	}
}

// TestProvisionPipeline_FinalActivateFailureDeprovisions is a regression test for
// audit finding #4: if FinalActivateSession fails after a live EC2 instance exists,
// the pipeline must terminate that instance rather than leaving it running and leaking cost.
func TestProvisionPipeline_FinalActivateFailureDeprovisions(t *testing.T) {
	finalActivateCalls, deprovCalls, stopCalls := 0, 0, 0
	ms := &mockStore{
		activateSessionFn: func(_ context.Context, in store.ActivateProvisionedSessionInput) (*model.Session, error) {
			return &model.Session{
				ID:                 in.SessionID,
				UserID:             in.UserID,
				Status:             model.SessionProvisioning,
				RelayAWSInstanceID: in.AWSInstanceID,
				PublicIP:           in.PublicIP,
			}, nil
		},
		finalActivateSessionFn: func(_ context.Context, _ string) error {
			finalActivateCalls++
			return errors.New("db write failed")
		},
		stopSessionFn: func(_ context.Context, userID, sessionID string) (*model.Session, error) {
			stopCalls++
			return &model.Session{ID: sessionID, UserID: userID, Status: model.SessionStopped}, nil
		},
	}
	mp := &mockProvisioner{
		provisionFn: func(_ context.Context, _ relay.ProvisionRequest) (relay.ProvisionResult, error) {
			return relay.ProvisionResult{
				AWSInstanceID: "i-leak-risk",
				AMIID:         "ami-123",
				InstanceType:  "t4g.small",
				PublicIP:      "198.51.100.50",
				SRTPort:       5000,
			}, nil
		},
		deprovisionFn: func(_ context.Context, req relay.DeprovisionRequest) error {
			deprovCalls++
			if req.AWSInstanceID != "i-leak-risk" {
				t.Errorf("unexpected instance id in deprovision: %s", req.AWSInstanceID)
			}
			return nil
		},
	}

	s := newTestServer(ms, mp)
	s.runProvisionPipeline("ses_final_fail", "usr_1", "us-east-1")

	if finalActivateCalls != 1 {
		t.Fatalf("expected 1 final activate call, got %d", finalActivateCalls)
	}
	if deprovCalls != 1 {
		t.Fatalf("expected 1 deprovision call (instance must not be leaked), got %d", deprovCalls)
	}
	if stopCalls != 1 {
		t.Fatalf("expected 1 stop call, got %d", stopCalls)
	}
}

func TestRelayHealth_MissingInstanceIDReturns400(t *testing.T) {
	_, router := NewRouter(testConfig(), &mockStore{}, &mockProvisioner{}, nil)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/health", jsonBody(map[string]any{
		"session_id":   "ses_1",
		"ingest_active": true,
		// instance_id omitted intentionally
	}))
	req.Header.Set("X-Relay-Auth", "relay-key")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusBadRequest {
		t.Fatalf("expected 400 for missing instance_id, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestRelayHealth_InstanceIDMismatchRejects(t *testing.T) {
	// Simulate a stale or spoofed relay reporting against the wrong session instance.
	ms := &mockStore{
		recordRelayHealthEventFn: func(_ context.Context, in store.RelayHealthInput) error {
			if in.InstanceID != "i-wrong" {
				return errors.New("unexpected instance id passed to store")
			}
			// Store rejects because aws_instance_id doesn't match session's relay instance.
			return store.ErrRelayHealthRejected
		},
	}

	_, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/health", jsonBody(map[string]any{
		"session_id":             "ses_1",
		"instance_id":            "i-wrong",
		"ingest_active":          true,
		"egress_active":          true,
		"session_uptime_seconds": 60,
	}))
	req.Header.Set("X-Relay-Auth", "relay-key")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusBadRequest {
		t.Fatalf("expected 400 for instance_id mismatch, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func testConfig() config.Config {
	return config.Config{
		JWTSecret:       "test-secret",
		RelaySharedKey:  "relay-key",
		DefaultRegion:   "us-east-1",
		SupportedRegion: []string{"us-east-1", "eu-west-1"},
		AWSInstanceType: "t4g.small",
	}
}

func testJWT(t *testing.T, secret, userID string) string {
	t.Helper()
	claims := jwt.MapClaims{
		"uid": userID,
		"exp": time.Now().Add(1 * time.Hour).Unix(),
		"iat": time.Now().Unix(),
	}
	tok := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	signed, err := tok.SignedString([]byte(secret))
	if err != nil {
		t.Fatalf("sign jwt: %v", err)
	}
	return signed
}

func jsonBody(v any) *bytes.Reader {
	b, _ := json.Marshal(v)
	return bytes.NewReader(b)
}
