package api

import (
    "context"
    "encoding/json"
    "net/http"
    "net/http/httptest"
    "testing"
    "time"

    "github.com/telemyapp/aegis-control-plane/internal/auth"
    "github.com/telemyapp/aegis-control-plane/internal/model"
    "github.com/telemyapp/aegis-control-plane/internal/store"
)

func TestAuthSession_ReturnsUserEntitlementUsageAndActiveSessions(t *testing.T) {
    now := time.Now().UTC()
    ms := &mockStore{
        getAuthSessionFn: func(_ context.Context, sessionID string) (*model.AuthSession, error) {
            return &model.AuthSession{ID: sessionID, UserID: "usr_1", ExpiresAt: now.Add(30 * time.Minute)}, nil
        },
        getUserFn: func(_ context.Context, userID string) (*model.User, error) {
            return &model.User{ID: userID, Email: "user@example.com", DisplayName: "telemy-user"}, nil
        },
        getRelayEntitlementFn: func(_ context.Context, userID string) (*model.RelayEntitlement, error) {
            return &model.RelayEntitlement{PlanTier: "pro", PlanStatus: "active", RelayAccessStatus: "enabled", MaxConcurrentConns: 3, ActiveManagedConns: 1, Allowed: true}, nil
        },
        getUsageCurrentFn: func(_ context.Context, userID string) (*model.UsageCurrent, error) {
            return &model.UsageCurrent{PlanTier: "pro", PlanStatus: "active", IncludedSeconds: 14400, ConsumedSeconds: 3600, RemainingSeconds: 10800}, nil
        },
        listActiveSessionsFn: func(_ context.Context, userID string) ([]model.Session, error) {
            return []model.Session{{ID: "ses_1", ConnectionID: "11111111-1111-1111-1111-111111111111", Status: model.SessionActive, PublicIP: "198.51.100.21", SRTPort: 5000, StreamToken: "streamtok"}}, nil
        },
    }

    _, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
    req := httptest.NewRequest(http.MethodGet, "/api/v1/auth/session", nil)
    req.Header.Set("Authorization", "Bearer "+testSessionJWT(t, "test-secret", "usr_1", "aut_1"))
    rr := httptest.NewRecorder()
    router.ServeHTTP(rr, req)

    if rr.Code != http.StatusOK {
        t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
    }

    var resp struct {
        User struct {
            ID string `json:"id"`
        } `json:"user"`
        Entitlement struct {
            RelayAccessStatus string `json:"relay_access_status"`
            ReasonCode        string `json:"reason_code"`
            MaxConcurrent     int    `json:"max_concurrent_conns"`
            ActiveManaged     int    `json:"active_managed_conns"`
        } `json:"entitlement"`
        ActiveRelay struct {
            SessionID string `json:"session_id"`
            Status    string `json:"status"`
        } `json:"active_relay"`
        ActiveSessions []struct {
            SessionID    string `json:"session_id"`
            ConnectionID string `json:"connection_id"`
            Status       string `json:"status"`
        } `json:"active_sessions"`
    }
    if err := json.NewDecoder(rr.Body).Decode(&resp); err != nil {
        t.Fatalf("decode response: %v", err)
    }
    if resp.User.ID != "usr_1" {
        t.Fatalf("unexpected user id: %s", resp.User.ID)
    }
    if resp.Entitlement.RelayAccessStatus != "enabled" || resp.Entitlement.ReasonCode != "ok" {
        t.Fatalf("unexpected entitlement: %+v", resp.Entitlement)
    }
    if resp.Entitlement.MaxConcurrent != 3 || resp.Entitlement.ActiveManaged != 1 {
        t.Fatalf("unexpected entitlement counts: %+v", resp.Entitlement)
    }
    if resp.ActiveRelay.SessionID != "ses_1" || resp.ActiveRelay.Status != "active" {
        t.Fatalf("unexpected active relay: %+v", resp.ActiveRelay)
    }
    if len(resp.ActiveSessions) != 1 || resp.ActiveSessions[0].ConnectionID == "" {
        t.Fatalf("unexpected active sessions: %+v", resp.ActiveSessions)
    }
}

func TestAuthRefresh_RotatesRefreshTokenAndReturnsAuthContext(t *testing.T) {
    now := time.Now().UTC()
    oldRefresh := "old_refresh_token"
    oldHash := auth.HashOpaqueToken(oldRefresh)
    rotated := false
    ms := &mockStore{
        getAuthSessionByHashFn: func(_ context.Context, refreshHash string) (*model.AuthSession, error) {
            if refreshHash != oldHash {
                t.Fatalf("unexpected refresh hash")
            }
            return &model.AuthSession{ID: "aut_1", UserID: "usr_1", ExpiresAt: now.Add(24 * time.Hour)}, nil
        },
        rotateAuthSessionFn: func(_ context.Context, sessionID, refreshHash string, expiresAt time.Time) (*model.AuthSession, error) {
            rotated = true
            if sessionID != "aut_1" || refreshHash == oldHash || refreshHash == "" {
                t.Fatalf("unexpected rotation input: session=%s refresh=%s", sessionID, refreshHash)
            }
            return &model.AuthSession{ID: sessionID, UserID: "usr_1", ExpiresAt: expiresAt}, nil
        },
        getUserFn: func(_ context.Context, userID string) (*model.User, error) {
            return &model.User{ID: userID, Email: "user@example.com", DisplayName: "telemy-user"}, nil
        },
        getRelayEntitlementFn: func(_ context.Context, userID string) (*model.RelayEntitlement, error) {
            return &model.RelayEntitlement{PlanTier: "pro", PlanStatus: "active", RelayAccessStatus: "enabled", Allowed: true}, nil
        },
        getUsageCurrentFn: func(_ context.Context, userID string) (*model.UsageCurrent, error) {
            return &model.UsageCurrent{PlanTier: "pro", PlanStatus: "active", IncludedSeconds: 14400, ConsumedSeconds: 3900, RemainingSeconds: 10500}, nil
        },
    }

    _, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
    req := httptest.NewRequest(http.MethodPost, "/api/v1/auth/refresh", jsonBody(map[string]any{"refresh_token": oldRefresh}))
    rr := httptest.NewRecorder()
    router.ServeHTTP(rr, req)

    if rr.Code != http.StatusOK {
        t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
    }
    if !rotated {
        t.Fatal("expected auth session rotation")
    }

    var resp struct {
        Auth struct {
            AccessToken  string `json:"cp_access_jwt"`
            RefreshToken string `json:"refresh_token"`
        } `json:"auth"`
        Entitlement struct {
            RelayAccessStatus string `json:"relay_access_status"`
        } `json:"entitlement"`
    }
    if err := json.NewDecoder(rr.Body).Decode(&resp); err != nil {
        t.Fatalf("decode response: %v", err)
    }
    if resp.Auth.AccessToken == "" || resp.Auth.RefreshToken == "" || resp.Auth.RefreshToken == oldRefresh {
        t.Fatalf("unexpected auth payload: %+v", resp.Auth)
    }
    if resp.Entitlement.RelayAccessStatus != "enabled" {
        t.Fatalf("unexpected entitlement payload: %+v", resp.Entitlement)
    }
}

func TestAuthLogout_RevokesCurrentSession(t *testing.T) {
    now := time.Now().UTC()
    revoked := false
    ms := &mockStore{
        getAuthSessionFn: func(_ context.Context, sessionID string) (*model.AuthSession, error) {
            return &model.AuthSession{ID: sessionID, UserID: "usr_1", ExpiresAt: now.Add(30 * time.Minute)}, nil
        },
        revokeAuthSessionFn: func(_ context.Context, sessionID string) error {
            revoked = true
            if sessionID != "aut_1" {
                t.Fatalf("unexpected session id: %s", sessionID)
            }
            return nil
        },
    }

    _, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
    req := httptest.NewRequest(http.MethodPost, "/api/v1/auth/logout", nil)
    req.Header.Set("Authorization", "Bearer "+testSessionJWT(t, "test-secret", "usr_1", "aut_1"))
    rr := httptest.NewRecorder()
    router.ServeHTTP(rr, req)

    if rr.Code != http.StatusNoContent {
        t.Fatalf("expected 204, got %d body=%s", rr.Code, rr.Body.String())
    }
    if !revoked {
        t.Fatal("expected auth session revoke call")
    }
}

func TestPluginLoginStart_ReturnsAttemptAndAuthorizeURL(t *testing.T) {
    ms := &mockStore{
        createPluginLoginAttemptFn: func(_ context.Context, in store.CreatePluginLoginAttemptInput) (*model.PluginLoginAttempt, error) {
            if in.ClientPlatform != "windows" || in.ClientVersion != "0.0.5" || in.DeviceName != "OBS Desktop" || in.PollTokenHash == "" {
                t.Fatalf("unexpected create input: %+v", in)
            }
            return &model.PluginLoginAttempt{ID: in.ID, PollTokenHash: in.PollTokenHash, Status: model.PluginLoginPending, ClientPlatform: in.ClientPlatform, ClientVersion: in.ClientVersion, DeviceName: in.DeviceName, ExpiresAt: in.ExpiresAt, CreatedAt: time.Now().UTC()}, nil
        },
    }

    _, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
    req := httptest.NewRequest(http.MethodPost, "/api/v1/auth/plugin/login/start", jsonBody(map[string]any{"client": map[string]any{"platform": "windows", "plugin_version": "0.0.5", "device_name": "OBS Desktop"}}))
    rr := httptest.NewRecorder()
    router.ServeHTTP(rr, req)

    if rr.Code != http.StatusCreated {
        t.Fatalf("expected 201, got %d body=%s", rr.Code, rr.Body.String())
    }
}

func TestPluginLoginPoll_PendingReturns202(t *testing.T) {
    pollToken := "poll-token"
    ms := &mockStore{
        getPluginLoginByPollFn: func(_ context.Context, attemptID, pollHash string) (*model.PluginLoginAttempt, error) {
            if attemptID != "pla_1" || pollHash != auth.HashOpaqueToken(pollToken) {
                t.Fatalf("unexpected poll lookup args")
            }
            return &model.PluginLoginAttempt{ID: attemptID, PollTokenHash: pollHash, Status: model.PluginLoginPending, ClientPlatform: "windows", ExpiresAt: time.Now().UTC().Add(5 * time.Minute)}, nil
        },
    }

    _, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
    req := httptest.NewRequest(http.MethodPost, "/api/v1/auth/plugin/login/poll", jsonBody(map[string]any{"login_attempt_id": "pla_1", "poll_token": pollToken}))
    rr := httptest.NewRecorder()
    router.ServeHTTP(rr, req)

    if rr.Code != http.StatusAccepted {
        t.Fatalf("expected 202, got %d body=%s", rr.Code, rr.Body.String())
    }
}

func TestPluginLoginCompleteThenPoll_IssuesTokens(t *testing.T) {
    pollToken := "poll-token"
    pollHash := auth.HashOpaqueToken(pollToken)
    userID := "usr_1"
    ms := &mockStore{
        finalizePluginLoginFn: func(_ context.Context, in store.FinalizePluginLoginAttemptInput) error {
            if in.AttemptID != "pla_1" || in.Status != model.PluginLoginCompleted || in.UserID != userID {
                t.Fatalf("unexpected finalize input: %+v", in)
            }
            return nil
        },
        getPluginLoginByPollFn: func(_ context.Context, attemptID, gotPollHash string) (*model.PluginLoginAttempt, error) {
            if attemptID != "pla_1" || gotPollHash != pollHash {
                t.Fatalf("unexpected poll lookup")
            }
            return &model.PluginLoginAttempt{ID: attemptID, PollTokenHash: gotPollHash, Status: model.PluginLoginCompleted, UserID: &userID, ClientPlatform: "windows", ClientVersion: "0.0.5", DeviceName: "OBS Desktop", ExpiresAt: time.Now().UTC().Add(5 * time.Minute)}, nil
        },
        claimPluginLoginFn: func(_ context.Context, attemptID, gotPollHash string, authIn store.CreateAuthSessionInput) (*model.PluginLoginAttempt, *model.AuthSession, error) {
            if attemptID != "pla_1" || gotPollHash != pollHash || authIn.UserID != userID || authIn.RefreshTokenHash == "" {
                t.Fatalf("unexpected claim input: attempt=%s poll=%s auth=%+v", attemptID, gotPollHash, authIn)
            }
            return &model.PluginLoginAttempt{ID: attemptID, PollTokenHash: gotPollHash, Status: model.PluginLoginCompleted, UserID: &userID, CompletedSessionID: &authIn.ID, ClientPlatform: "windows", ExpiresAt: time.Now().UTC().Add(5 * time.Minute)}, &model.AuthSession{ID: authIn.ID, UserID: userID, ExpiresAt: authIn.ExpiresAt}, nil
        },
        getUserFn: func(_ context.Context, userID string) (*model.User, error) {
            return &model.User{ID: userID, Email: "user@example.com", DisplayName: "telemy-user"}, nil
        },
        getRelayEntitlementFn: func(_ context.Context, userID string) (*model.RelayEntitlement, error) {
            return &model.RelayEntitlement{PlanTier: "pro", PlanStatus: "active", RelayAccessStatus: "enabled", Allowed: true}, nil
        },
        getUsageCurrentFn: func(_ context.Context, userID string) (*model.UsageCurrent, error) {
            return &model.UsageCurrent{PlanTier: "pro", PlanStatus: "active", IncludedSeconds: 14400, ConsumedSeconds: 0, RemainingSeconds: 14400}, nil
        },
    }

    _, router := NewRouter(testConfig(), ms, &mockProvisioner{}, nil)
    completeReq := httptest.NewRequest(http.MethodPost, "/api/v1/auth/plugin/login/complete", jsonBody(map[string]any{"login_attempt_id": "pla_1", "outcome": "completed", "user_id": userID}))
    completeReq.Header.Set("X-Plugin-Login-Auth", "plugin-login-key")
    completeRR := httptest.NewRecorder()
    router.ServeHTTP(completeRR, completeReq)
    if completeRR.Code != http.StatusNoContent {
        t.Fatalf("expected 204 from complete, got %d body=%s", completeRR.Code, completeRR.Body.String())
    }

    pollReq := httptest.NewRequest(http.MethodPost, "/api/v1/auth/plugin/login/poll", jsonBody(map[string]any{"login_attempt_id": "pla_1", "poll_token": pollToken}))
    pollRR := httptest.NewRecorder()
    router.ServeHTTP(pollRR, pollReq)
    if pollRR.Code != http.StatusOK {
        t.Fatalf("expected 200, got %d body=%s", pollRR.Code, pollRR.Body.String())
    }

    var resp struct {
        Status string `json:"status"`
        Auth   struct {
            AccessToken  string `json:"cp_access_jwt"`
            RefreshToken string `json:"refresh_token"`
        } `json:"auth"`
    }
    if err := json.NewDecoder(pollRR.Body).Decode(&resp); err != nil {
        t.Fatalf("decode response: %v", err)
    }
    if resp.Status != "completed" || resp.Auth.AccessToken == "" || resp.Auth.RefreshToken == "" {
        t.Fatalf("unexpected poll response: %+v", resp)
    }
}

func testSessionJWT(t *testing.T, secret, userID, sessionID string) string {
    t.Helper()
    signed, err := auth.SignSessionJWT(secret, userID, sessionID, time.Hour)
    if err != nil {
        t.Fatalf("sign session jwt: %v", err)
    }
    return signed
}
