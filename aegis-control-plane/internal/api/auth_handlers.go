package api

import (
	"context"
	"errors"
	"net/http"
	"net/url"
	"time"

	"github.com/google/uuid"
	"github.com/telemyapp/aegis-control-plane/internal/auth"
	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

type authRefreshRequest struct {
	RefreshToken string `json:"refresh_token"`
}

type pluginLoginStartRequest struct {
	Client struct {
		Platform      string `json:"platform"`
		PluginVersion string `json:"plugin_version"`
		DeviceName    string `json:"device_name"`
	} `json:"client"`
}

type pluginLoginPollRequest struct {
	LoginAttemptID string `json:"login_attempt_id"`
	PollToken      string `json:"poll_token"`
}

type pluginLoginCompleteRequest struct {
	LoginAttemptID string `json:"login_attempt_id"`
	Outcome        string `json:"outcome"`
	UserID         string `json:"user_id"`
	ReasonCode     string `json:"reason_code"`
}

func (s *Server) handlePluginLoginStart(w http.ResponseWriter, r *http.Request) {
	var req pluginLoginStartRequest
	if code, msg := decodeJSON(r, &req); code != 0 {
		writeAPIError(w, code, "invalid_request", msg)
		return
	}
	if req.Client.Platform == "" {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "client.platform is required")
		return
	}
	if len(req.Client.Platform) > 32 || len(req.Client.PluginVersion) > 32 || len(req.Client.DeviceName) > 128 {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "client fields too long")
		return
	}

	pollToken, err := auth.GenerateOpaqueToken(24)
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to generate poll token")
		return
	}
	attemptID := "pla_" + uuid.NewString()
	expiresAt := time.Now().UTC().Add(s.cfg.PluginLoginAttemptTTL)
	attempt, err := s.store.CreatePluginLoginAttempt(r.Context(), store.CreatePluginLoginAttemptInput{
		ID:             attemptID,
		PollTokenHash:  auth.HashOpaqueToken(pollToken),
		ClientPlatform: req.Client.Platform,
		ClientVersion:  req.Client.PluginVersion,
		DeviceName:     req.Client.DeviceName,
		ExpiresAt:      expiresAt,
	})
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to create login attempt")
		return
	}

	authorizeURL := s.cfg.AuthPublicBaseURL + "/login/plugin?attempt=" + url.QueryEscape(attempt.ID)
	writeJSON(w, http.StatusCreated, map[string]any{
		"login_attempt_id":      attempt.ID,
		"authorize_url":         authorizeURL,
		"poll_token":            pollToken,
		"expires_at":            attempt.ExpiresAt.UTC().Format(time.RFC3339),
		"poll_interval_seconds": s.cfg.PluginLoginPollIntervalSec,
	})
}

func (s *Server) handlePluginLoginPoll(w http.ResponseWriter, r *http.Request) {
	var req pluginLoginPollRequest
	if code, msg := decodeJSON(r, &req); code != 0 {
		writeAPIError(w, code, "invalid_request", msg)
		return
	}
	if req.LoginAttemptID == "" || req.PollToken == "" {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "login_attempt_id and poll_token are required")
		return
	}

	pollHash := auth.HashOpaqueToken(req.PollToken)
	attempt, err := s.store.GetPluginLoginAttemptByPollToken(r.Context(), req.LoginAttemptID, pollHash)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusNotFound, "not_found", "login attempt not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to load login attempt")
		return
	}
	if attempt.IsExpired(time.Now().UTC()) && attempt.Status == model.PluginLoginPending {
		_ = s.store.MarkPluginLoginAttemptExpired(r.Context(), attempt.ID)
		writeAPIError(w, http.StatusGone, "login_attempt_expired", "login attempt expired")
		return
	}

	switch attempt.Status {
	case model.PluginLoginPending:
		writeJSON(w, http.StatusAccepted, map[string]any{"status": "pending"})
		return
	case model.PluginLoginDenied:
		reason := attempt.DenyReasonCode
		if reason == "" {
			reason = "login_denied"
		}
		writeAPIErrorWithReason(w, http.StatusForbidden, "login_denied", "login denied", reason)
		return
	case model.PluginLoginExpired:
		writeAPIError(w, http.StatusGone, "login_attempt_expired", "login attempt expired")
		return
	case model.PluginLoginCompleted:
		if attempt.CompletedSessionID != nil {
			writeAPIError(w, http.StatusConflict, "login_already_claimed", "login attempt already claimed")
			return
		}
	default:
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "invalid login attempt state")
		return
	}

	if attempt.UserID == nil || *attempt.UserID == "" {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "login attempt missing user")
		return
	}
	refreshToken, err := auth.GenerateOpaqueToken(32)
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to generate refresh token")
		return
	}
	sessionID := "aut_" + uuid.NewString()
	authSess, _, err := s.issueCompletedPluginLoginAttempt(r.Context(), attempt, pollHash, sessionID, refreshToken)
	if err != nil {
		if errors.Is(err, store.ErrConflict) {
			writeAPIError(w, http.StatusConflict, "login_already_claimed", "login attempt already claimed")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to issue auth session")
		return
	}
	cpAccessJWT, err := auth.SignSessionJWT(s.cfg.JWTSecret, authSess.UserID, authSess.ID, s.cfg.AuthAccessTTL)
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to issue access token")
		return
	}
	resp, err := s.buildAuthSessionResponse(r.Context(), authSess.UserID, true)
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to build auth response")
		return
	}
	resp["status"] = "completed"
	resp["auth"] = map[string]any{
		"cp_access_jwt": cpAccessJWT,
		"refresh_token": refreshToken,
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *Server) handlePluginLoginComplete(w http.ResponseWriter, r *http.Request) {
	var req pluginLoginCompleteRequest
	if code, msg := decodeJSON(r, &req); code != 0 {
		writeAPIError(w, code, "invalid_request", msg)
		return
	}
	if req.LoginAttemptID == "" {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "login_attempt_id is required")
		return
	}
	switch req.Outcome {
	case string(model.PluginLoginCompleted):
		if req.UserID == "" {
			writeAPIError(w, http.StatusBadRequest, "invalid_request", "user_id is required for completed outcome")
			return
		}
	case string(model.PluginLoginDenied):
		if req.ReasonCode == "" {
			req.ReasonCode = "login_denied"
		}
	default:
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "outcome must be completed or denied")
		return
	}

	err := s.store.FinalizePluginLoginAttempt(r.Context(), store.FinalizePluginLoginAttemptInput{
		AttemptID:      req.LoginAttemptID,
		Status:         model.PluginLoginAttemptStatus(req.Outcome),
		UserID:         req.UserID,
		DenyReasonCode: req.ReasonCode,
	})
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusNotFound, "not_found", "login attempt not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to finalize login attempt")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleAuthSession(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}

	resp, err := s.buildAuthSessionResponse(r.Context(), userID, true)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusUnauthorized, "unauthorized", "user not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to load auth session")
		return
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *Server) handleAuthRefresh(w http.ResponseWriter, r *http.Request) {
	var req authRefreshRequest
	if code, msg := decodeJSON(r, &req); code != 0 {
		writeAPIError(w, code, "invalid_request", msg)
		return
	}
	if req.RefreshToken == "" {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "refresh_token is required")
		return
	}

	refreshHash := auth.HashOpaqueToken(req.RefreshToken)
	sess, err := s.store.GetAuthSessionByRefreshHash(r.Context(), refreshHash)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusUnauthorized, "unauthorized", "invalid refresh token")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to load auth session")
		return
	}
	if !sess.IsActive(time.Now().UTC()) {
		writeAPIError(w, http.StatusGone, "session_expired", "auth session expired")
		return
	}

	newRefreshToken, err := auth.GenerateOpaqueToken(32)
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to generate refresh token")
		return
	}
	newExpiresAt := time.Now().UTC().Add(s.cfg.AuthRefreshTTL)
	sess, err = s.store.RotateAuthSession(r.Context(), sess.ID, auth.HashOpaqueToken(newRefreshToken), newExpiresAt)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusGone, "session_expired", "auth session expired")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to rotate auth session")
		return
	}

	cpAccessJWT, err := auth.SignSessionJWT(s.cfg.JWTSecret, sess.UserID, sess.ID, s.cfg.AuthAccessTTL)
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to issue access token")
		return
	}

	resp, err := s.buildAuthSessionResponse(r.Context(), sess.UserID, false)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusUnauthorized, "unauthorized", "user not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to load auth session")
		return
	}
	resp["auth"] = map[string]any{
		"cp_access_jwt": cpAccessJWT,
		"refresh_token": newRefreshToken,
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *Server) handleAuthLogout(w http.ResponseWriter, r *http.Request) {
	sessionID, ok := auth.SessionIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing auth session")
		return
	}
	if err := s.store.RevokeAuthSession(r.Context(), sessionID); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to revoke auth session")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) buildAuthSessionResponse(ctx context.Context, userID string, includeActiveRelay bool) (map[string]any, error) {
	user, err := s.store.GetUser(ctx, userID)
	if err != nil {
		return nil, err
	}
	entitlement, err := s.store.GetRelayEntitlement(ctx, userID)
	if err != nil {
		return nil, err
	}
	usage, err := s.store.GetUsageCurrent(ctx, userID)
	if err != nil {
		return nil, err
	}

	resp := map[string]any{
		"user": map[string]any{
			"id":           user.ID,
			"email":        user.Email,
			"display_name": user.DisplayName,
		},
		"entitlement": map[string]any{
			"relay_access_status": relayAccessStatus(entitlement),
			"reason_code":         relayAccessReason(entitlement),
			"plan_tier":           entitlement.PlanTier,
			"plan_status":         entitlement.PlanStatus,
		},
		"usage": map[string]any{
			"included_seconds":  usage.IncludedSeconds,
			"consumed_seconds":  usage.ConsumedSeconds,
			"remaining_seconds": usage.RemainingSeconds,
			"overage_seconds":   usage.OverageSeconds,
		},
	}

	if includeActiveRelay {
		active, err := s.store.GetActiveSession(ctx, userID)
		if err != nil {
			return nil, err
		}
		if active == nil {
			resp["active_relay"] = nil
		} else {
			resp["active_relay"] = map[string]any{
				"session_id": active.ID,
				"status":     string(active.Status),
			}
		}
	}

	return resp, nil
}

func relayAccessStatus(entitlement *model.RelayEntitlement) string {
	if entitlement != nil && entitlement.Allowed {
		return "enabled"
	}
	return "disabled"
}

func relayAccessReason(entitlement *model.RelayEntitlement) string {
	if entitlement == nil {
		return "entitlement_denied"
	}
	if entitlement.Allowed {
		return "ok"
	}
	if entitlement.ReasonCode != "" {
		return entitlement.ReasonCode
	}
	return "entitlement_denied"
}

func (s *Server) issueCompletedPluginLoginAttempt(ctx context.Context, attempt *model.PluginLoginAttempt, pollTokenHash, sessionID, refreshToken string) (*model.AuthSession, *model.PluginLoginAttempt, error) {
	updatedAttempt, authSess, err := s.store.ClaimCompletedPluginLoginAttempt(ctx, attempt.ID, pollTokenHash, store.CreateAuthSessionInput{
		ID:               sessionID,
		UserID:           *attempt.UserID,
		RefreshTokenHash: auth.HashOpaqueToken(refreshToken),
		ClientPlatform:   attempt.ClientPlatform,
		ClientVersion:    attempt.ClientVersion,
		DeviceName:       attempt.DeviceName,
		ExpiresAt:        time.Now().UTC().Add(s.cfg.AuthRefreshTTL),
	})
	return authSess, updatedAttempt, err
}
