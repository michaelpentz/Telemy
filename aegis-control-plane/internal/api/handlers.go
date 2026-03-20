package api

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"math/big"
	"net/http"
	"regexp"
	"slices"
	"time"

	"github.com/telemyapp/aegis-control-plane/internal/auth"
	"github.com/telemyapp/aegis-control-plane/internal/metrics"
	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/relay"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

// decodeJSON decodes a JSON request body into dst, rejecting unknown fields.
// Returns an appropriate HTTP status code and error message on failure.
// A zero status indicates success.
func decodeJSON(r *http.Request, dst any) (status int, msg string) {
	r.Body = http.MaxBytesReader(nil, r.Body, 1<<20) // 1MB limit
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		var maxBytesErr *http.MaxBytesError
		if errors.As(err, &maxBytesErr) {
			return http.StatusRequestEntityTooLarge, "request body too large"
		}
		return http.StatusBadRequest, "invalid JSON payload"
	}
	return 0, ""
}

var uuidRe = regexp.MustCompile(`^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$`)

type relayStartRequest struct {
	RegionPreference string `json:"region_preference"`
	ConnectionID     string `json:"connection_id"`
	ClientContext    struct {
		OBSConnected bool   `json:"obs_connected"`
		Mode         string `json:"mode"`
		RequestedBy  string `json:"requested_by"`
	} `json:"client_context"`
}

type relayStopRequest struct {
	SessionID    string `json:"session_id"`
	ConnectionID string `json:"connection_id"`
	Reason       string `json:"reason"`
}

type relayHealthRequest struct {
	SessionID            string `json:"session_id"`
	InstanceID           string `json:"instance_id"`
	IngestActive         bool   `json:"ingest_active"`
	EgressActive         bool   `json:"egress_active"`
	SessionUptimeSeconds int    `json:"session_uptime_seconds"`
	ObservedAt           string `json:"observed_at"`
}

func (s *Server) handleRelayStart(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}

	entitlement, err := s.store.GetRelayEntitlement(r.Context(), userID)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIErrorWithReason(w, http.StatusForbidden, "entitlement_denied", "relay access is not enabled for this account", "user_not_found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to verify relay entitlement")
		return
	}
	if !entitlement.Allowed {
		reason := entitlement.ReasonCode
		if reason == "" {
			reason = "entitlement_denied"
		}
		code := "entitlement_denied"
		if reason == "connection_limit_reached" {
			code = "connection_limit_reached"
		}
		writeAPIErrorWithReason(w, http.StatusForbidden, code, "relay access is not enabled for this account", reason)
		return
	}
	sessionProvisioner := s.provisioner
	providerName := s.cfg.RelayProvider

	idemRaw := r.Header.Get("Idempotency-Key")
	if idemRaw == "" {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "Idempotency-Key is required")
		return
	}
	idem, err := parseIdempotencyKey(idemRaw)
	if err != nil {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "Idempotency-Key must be uuid-v4")
		return
	}

	var req relayStartRequest
	if code, msg := decodeJSON(r, &req); code != 0 {
		writeAPIError(w, code, "invalid_request", msg)
		return
	}
	if len(req.RegionPreference) > 64 {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "region_preference too long")
		return
	}
	if req.ConnectionID != "" && !uuidRe.MatchString(req.ConnectionID) {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "connection_id must be uuid-v4")
		return
	}

	region := s.resolveRegion(req.RegionPreference)
	requestedBy := req.ClientContext.RequestedBy
	if requestedBy == "" {
		requestedBy = "dashboard"
	}

	hash, err := store.HashJSON(req)
	if err != nil {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "failed to hash request")
		return
	}

	sess, created, err := s.store.StartOrGetSession(r.Context(), store.StartInput{
		UserID:         userID,
		ConnectionID:   req.ConnectionID,
		Region:         region,
		RequestedBy:    requestedBy,
		IdempotencyKey: idem,
		RequestHash:    hash,
	})
	if err != nil {
		switch {
		case errors.Is(err, store.ErrIdempotencyMismatch):
			writeAPIError(w, http.StatusConflict, "idempotency_mismatch", "same key used with different payload")
		default:
			writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to start relay session")
		}
		return
	}

	if created {
		if err := s.store.UpdateProvisionStep(r.Context(), sess.ID, model.StepLaunchingInstance); err != nil {
			log.Printf("relay_start: failed setting initial provision step session_id=%s err=%v", sess.ID, err)
		}
		sess.ProvisionStep = model.StepLaunchingInstance
		sessCopy := *sess
		s.wg.Add(1)
		go func(sess model.Session) {
			defer s.wg.Done()
			s.runProvisionPipeline(s.appCtx, &sess, sessionProvisioner, providerName)
		}(sessCopy)
	}

	status := http.StatusOK
	if created {
		status = http.StatusCreated
	}
	writeJSON(w, status, map[string]any{"session": toSessionResponse(sess)})
}

func (s *Server) compensateRelayStartProvisioned(ctx context.Context, sess *model.Session, userID string, prov relay.ProvisionResult) {
	if deprovErr := s.provisioner.Deprovision(ctx, relay.DeprovisionRequest{
		SessionID:  sess.ID,
		UserID:     userID,
		Region:     sess.Region,
		InstanceID: prov.InstanceID,
	}); deprovErr != nil {
		log.Printf("relay_start_compensation deprovision_failed session_id=%s user_id=%s instance_id=%s err=%v", sess.ID, userID, prov.InstanceID, deprovErr)
	}
	if _, stopErr := s.store.StopSession(ctx, userID, sess.ID); stopErr != nil {
		log.Printf("relay_start_compensation stop_session_failed session_id=%s user_id=%s err=%v", sess.ID, userID, stopErr)
	}
}

func (s *Server) runProvisionPipeline(appCtx context.Context, sess *model.Session, provisioner relay.Provisioner, providerName string) {
	ctx, cancel := context.WithTimeout(appCtx, 5*time.Minute)
	defer cancel()
	sessionID, userID, region := sess.ID, sess.UserID, sess.Region

	probe := s.probeReady
	if probe == nil {
		probe = s.probeUntilReady
	}
	dwell := s.dwell
	if dwell == nil {
		dwell = stepDwell
	}

	if err := s.store.UpdateProvisionStep(ctx, sessionID, model.StepLaunchingInstance); err != nil {
		log.Printf("provision_pipeline: update_step_failed session_id=%s step=%s err=%v", sessionID, model.StepLaunchingInstance, err)
	}

	provisionStart := time.Now()
	prov, err := provisioner.Provision(ctx, relay.ProvisionRequest{
		SessionID:   sessionID,
		UserID:      userID,
		Region:      region,
		StreamToken: sess.StreamToken,
	})
	durMS := float64(time.Since(provisionStart).Milliseconds())
	labels := map[string]string{
		"provider": providerName,
		"region":   region,
	}
	if err != nil {
		log.Printf("metric=relay_provision_latency_ms session_id=%s user_id=%s region=%s value=%d status=error", sessionID, userID, region, time.Since(provisionStart).Milliseconds())
		labels["status"] = "error"
		metrics.Default().IncCounter("aegis_relay_provision_total", labels)
		metrics.Default().ObserveHistogram("aegis_relay_provision_latency_ms", durMS, labels)
		s.deprovisionAndStop(ctx, sessionID, userID, region, "", provisioner)
		return
	}
	log.Printf("metric=relay_provision_latency_ms session_id=%s user_id=%s region=%s value=%d status=ok", sessionID, userID, region, time.Since(provisionStart).Milliseconds())
	labels["status"] = "ok"
	metrics.Default().IncCounter("aegis_relay_provision_total", labels)
	metrics.Default().ObserveHistogram("aegis_relay_provision_latency_ms", durMS, labels)

	if err := s.store.UpdateProvisionStep(ctx, sessionID, model.StepWaitingForInstance); err != nil {
		log.Printf("provision_pipeline: update_step_failed session_id=%s step=%s err=%v", sessionID, model.StepWaitingForInstance, err)
	}
	// Minimum dwell so clients polling at 2s can see each step
	dwell(ctx, 3*time.Second)

	// EIP allocation/association is now handled internally by AWSProvisioner.Provision().
	// The returned prov.PublicIP is already the stable EIP (if available) or the
	// auto-assigned IP. No provider-specific logic needed here.

	pairToken, err := generatePairToken(8)
	if err != nil {
		log.Printf("provision_pipeline: pair_token_failed session_id=%s err=%v", sessionID, err)
		s.deprovisionAndStop(ctx, sessionID, userID, region, prov.InstanceID, provisioner)
		return
	}
	relayWSToken, err := generateRelayWSToken()
	if err != nil {
		log.Printf("provision_pipeline: relay_ws_token_failed session_id=%s err=%v", sessionID, err)
		s.deprovisionAndStop(ctx, sessionID, userID, region, prov.InstanceID, provisioner)
		return
	}

	if _, err := s.store.ActivateProvisionedSession(ctx, store.ActivateProvisionedSessionInput{
		UserID:       userID,
		SessionID:    sessionID,
		Region:       region,
		InstanceID:   prov.InstanceID,
		AMIID:        prov.AMIID,
		InstanceType: prov.InstanceType,
		PublicIP:     provisionedPublicIP(providerName, prov.PublicIP),
		SRTPort:      prov.SRTPort,
		PairToken:    pairToken,
		RelayWSToken: relayWSToken,
	}); err != nil {
		log.Printf("provision_pipeline: activate_failed session_id=%s err=%v", sessionID, err)
		s.deprovisionAndStop(ctx, sessionID, userID, region, prov.InstanceID, provisioner)
		return
	}

	if providerName == "byor" {
		if err := s.store.UpdateProvisionStep(ctx, sessionID, model.StepReady); err != nil {
			log.Printf("provision_pipeline: update_step_failed session_id=%s step=%s err=%v", sessionID, model.StepReady, err)
		}
		if err := s.store.FinalActivateSession(ctx, sessionID); err != nil {
			log.Printf("provision_pipeline: final_activate_failed session_id=%s err=%v", sessionID, err)
			s.deprovisionAndStop(ctx, sessionID, userID, region, prov.InstanceID, provisioner)
			return
		}
		log.Printf("provision_pipeline: completed session_id=%s", sessionID)
		return
	}

	if err := s.store.UpdateProvisionStep(ctx, sessionID, model.StepStartingDocker); err != nil {
		log.Printf("provision_pipeline: update_step_failed session_id=%s step=%s err=%v", sessionID, model.StepStartingDocker, err)
	}
	dwell(ctx, 3*time.Second)
	healthURL := fmt.Sprintf("http://%s:8090/health", prov.PublicIP)
	if !probe(ctx, healthURL) {
		log.Printf("provision_pipeline: health_probe_timeout session_id=%s", sessionID)
		s.deprovisionAndStop(ctx, sessionID, userID, region, prov.InstanceID, provisioner)
		return
	}

	if err := s.store.UpdateProvisionStep(ctx, sessionID, model.StepStartingContainers); err != nil {
		log.Printf("provision_pipeline: update_step_failed session_id=%s step=%s err=%v", sessionID, model.StepStartingContainers, err)
	}
	dwell(ctx, 3*time.Second)

	if err := s.store.UpdateProvisionStep(ctx, sessionID, model.StepCreatingStream); err != nil {
		log.Printf("provision_pipeline: update_step_failed session_id=%s step=%s err=%v", sessionID, model.StepCreatingStream, err)
	}
	dwell(ctx, 3*time.Second)

	// Stream is auto-created by user-data script shortly after containers start.
	// Probe /health again to confirm the backend is still responsive (no auth needed).
	if !probe(ctx, healthURL) {
		log.Printf("provision_pipeline: post_stream_health_probe_timeout session_id=%s", sessionID)
		s.deprovisionAndStop(ctx, sessionID, userID, region, prov.InstanceID, provisioner)
		return
	}

	if err := s.store.UpdateProvisionStep(ctx, sessionID, model.StepReady); err != nil {
		log.Printf("provision_pipeline: update_step_failed session_id=%s step=%s err=%v", sessionID, model.StepReady, err)
	}
	// Let clients see "Ready 6/6" before flipping to active
	dwell(ctx, 3*time.Second)

	if slug, slugErr := s.store.GetUserRelaySlug(ctx, userID); slugErr == nil && slug != "" && s.dns != nil {
		if dnsErr := s.dns.CreateOrUpdateRecord(slug, prov.PublicIP); dnsErr != nil {
			log.Printf("dns: create record failed session_id=%s slug=%s err=%v", sessionID, slug, dnsErr)
		}
	}

	// Final state transition: provisioning â†’ active. If this fails, the live EC2 instance
	// must be terminated to prevent cost leakage from a permanently stuck session.
	if err := s.store.FinalActivateSession(ctx, sessionID); err != nil {
		log.Printf("provision_pipeline: final_activate_failed session_id=%s err=%v", sessionID, err)
		s.deprovisionAndStop(ctx, sessionID, userID, region, prov.InstanceID, provisioner)
		return
	}
	log.Printf("provision_pipeline: completed session_id=%s", sessionID)
}

// stepDwell pauses so clients polling at 2s intervals can see each provision step.
func stepDwell(ctx context.Context, d time.Duration) {
	select {
	case <-ctx.Done():
	case <-time.After(d):
	}
}

func (s *Server) probeUntilReady(ctx context.Context, url string) bool {
	client := &http.Client{Timeout: 3 * time.Second}
	for {
		select {
		case <-ctx.Done():
			return false
		default:
		}

		resp, err := client.Get(url)
		if err == nil {
			resp.Body.Close()
			if resp.StatusCode >= 200 && resp.StatusCode < 300 {
				return true
			}
		}

		select {
		case <-ctx.Done():
			return false
		case <-time.After(2 * time.Second):
		}
	}
}

func (s *Server) deprovisionAndStop(ctx context.Context, sessionID, userID, region, instanceID string, provisioner relay.Provisioner) {
	if instanceID != "" {
		if err := provisioner.Deprovision(ctx, relay.DeprovisionRequest{
			SessionID:  sessionID,
			UserID:     userID,
			Region:     region,
			InstanceID: instanceID,
		}); err != nil {
			log.Printf("provision_pipeline: deprovision_failed session_id=%s err=%v", sessionID, err)
		}
	}
	if _, err := s.store.StopSession(ctx, userID, sessionID); err != nil {
		log.Printf("provision_pipeline: stop_failed session_id=%s err=%v", sessionID, err)
	}
}

func (s *Server) handleRelayActive(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}
	sess, err := s.store.GetActiveSession(r.Context(), userID)
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to query active session")
		return
	}
	if sess == nil {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"session": toSessionResponse(sess)})
}

func (s *Server) handleRelayStop(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}

	var req relayStopRequest
	if code, msg := decodeJSON(r, &req); code != 0 {
		writeAPIError(w, code, "invalid_request", msg)
		return
	}
	if len(req.Reason) > 256 {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "reason too long")
		return
	}
	if req.ConnectionID != "" && !uuidRe.MatchString(req.ConnectionID) {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "connection_id must be uuid-v4")
		return
	}

	var curr *model.Session
	var err error
	switch {
	case req.SessionID != "":
		curr, err = s.store.GetSessionByID(r.Context(), userID, req.SessionID)
	case req.ConnectionID != "":
		curr, err = s.store.GetActiveSessionByConnection(r.Context(), userID, req.ConnectionID)
		if err == nil && curr == nil {
			err = store.ErrNotFound
		}
	default:
		curr, err = s.store.GetActiveSession(r.Context(), userID)
		if err == nil && curr == nil {
			err = store.ErrNotFound
		}
	}
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusNotFound, "not_found", "session not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to query session")
		return
	}
	if curr.Status != model.SessionStopped && curr.RelayInstanceID != "" {
		deprovStart := time.Now()
		if err := s.provisioner.Deprovision(r.Context(), relay.DeprovisionRequest{
			SessionID:  curr.ID,
			UserID:     curr.UserID,
			Region:     curr.Region,
			InstanceID: curr.RelayInstanceID,
		}); err != nil {
			durMS := float64(time.Since(deprovStart).Milliseconds())
			log.Printf("metric=relay_deprovision_latency_ms session_id=%s user_id=%s region=%s value=%d status=error", curr.ID, curr.UserID, curr.Region, time.Since(deprovStart).Milliseconds())
			labels := map[string]string{
				"provider": s.cfg.RelayProvider,
				"region":   curr.Region,
				"status":   "error",
			}
			metrics.Default().IncCounter("aegis_relay_deprovision_total", labels)
			metrics.Default().ObserveHistogram("aegis_relay_deprovision_latency_ms", durMS, labels)
			writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to terminate relay instance")
			return
		}
		durMS := float64(time.Since(deprovStart).Milliseconds())
		log.Printf("metric=relay_deprovision_latency_ms session_id=%s user_id=%s region=%s value=%d status=ok", curr.ID, curr.UserID, curr.Region, time.Since(deprovStart).Milliseconds())
		labels := map[string]string{
			"provider": s.cfg.RelayProvider,
			"region":   curr.Region,
			"status":   "ok",
		}
		metrics.Default().IncCounter("aegis_relay_deprovision_total", labels)
		metrics.Default().ObserveHistogram("aegis_relay_deprovision_latency_ms", durMS, labels)

		// DNS record is NOT deleted â€” EIP-backed records are permanent.
		// The record stays pointed at the user's Elastic IP across provision cycles.
	}

	sess, err := s.store.StopSession(r.Context(), userID, curr.ID)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusNotFound, "not_found", "session not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to stop session")
		return
	}
	stoppedAt := time.Now().UTC().Format(time.RFC3339)
	if sess.StoppedAt != nil {
		stoppedAt = sess.StoppedAt.UTC().Format(time.RFC3339)
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"session_id": sess.ID,
		"status":     string(sess.Status),
		"stopped_at": stoppedAt,
	})
}

func (s *Server) handleRelayManifest(w http.ResponseWriter, r *http.Request) {
	type regionDef struct {
		Region              string `json:"region"`
		AMIID               string `json:"ami_id"`
		DefaultInstanceType string `json:"default_instance_type"`
		UpdatedAt           string `json:"updated_at"`
	}
	manifest, err := s.store.ListRelayManifest(r.Context())
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to read relay manifest")
		return
	}
	if len(manifest) == 0 {
		writeAPIError(w, http.StatusServiceUnavailable, "manifest_unavailable", "relay manifest is not configured")
		return
	}
	regions := make([]regionDef, 0, len(manifest))
	for _, entry := range manifest {
		regions = append(regions, regionDef{
			Region:              entry.Region,
			AMIID:               entry.AMIID,
			DefaultInstanceType: entry.DefaultInstanceType,
			UpdatedAt:           entry.UpdatedAt.UTC().Format(time.RFC3339),
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"regions": regions})
}

func (s *Server) handleUsageCurrent(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}
	usage, err := s.store.GetUsageCurrent(r.Context(), userID)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusNotFound, "not_found", "user usage not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to query usage")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"plan_tier":         usage.PlanTier,
		"cycle_start":       usage.CycleStart.UTC().Format(time.RFC3339),
		"cycle_end":         usage.CycleEnd.UTC().Format(time.RFC3339),
		"included_seconds":  usage.IncludedSeconds,
		"consumed_seconds":  usage.ConsumedSeconds,
		"remaining_seconds": usage.RemainingSeconds,
		"overage_seconds":   usage.OverageSeconds,
	})
}

func (s *Server) handleRelayHealth(w http.ResponseWriter, r *http.Request) {
	var req relayHealthRequest
	if code, msg := decodeJSON(r, &req); code != 0 {
		writeAPIError(w, code, "invalid_request", msg)
		return
	}
	if req.SessionID == "" || req.InstanceID == "" {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "invalid relay health payload")
		return
	}
	if req.SessionUptimeSeconds < 0 || req.SessionUptimeSeconds > 604800 {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "session_uptime_seconds out of range")
		return
	}

	observedAt := time.Now().UTC()
	if req.ObservedAt != "" {
		t, err := time.Parse(time.RFC3339, req.ObservedAt)
		if err != nil {
			writeAPIError(w, http.StatusBadRequest, "invalid_request", "observed_at must be RFC3339")
			return
		}
		observedAt = t.UTC()
	}
	raw, _ := json.Marshal(req)

	err := s.store.RecordRelayHealth(r.Context(), store.RelayHealthInput{
		SessionID:            req.SessionID,
		InstanceID:           req.InstanceID,
		ObservedAt:           observedAt,
		IngestActive:         req.IngestActive,
		EgressActive:         req.EgressActive,
		SessionUptimeSeconds: req.SessionUptimeSeconds,
		RawPayload:           raw,
	})
	if err != nil {
		if errors.Is(err, store.ErrRelayHealthRejected) {
			writeAPIError(w, http.StatusBadRequest, "invalid_request", "relay health rejected")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to record relay health")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) resolveRegion(pref string) string {
	if pref == "" || pref == "auto" {
		return s.cfg.DefaultRegion
	}
	if slices.Contains(s.cfg.SupportedRegion, pref) {
		return pref
	}
	return s.cfg.DefaultRegion
}

func toSessionResponse(sess *model.Session) map[string]any {
	relayMap := map[string]any{
		"public_ip": sess.PublicIP,
		"srt_port":  sess.SRTPort,
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
			"stream_token":   sess.StreamToken,
		},
		"timers": map[string]any{
			"grace_window_seconds": sess.GraceWindowSeconds,
			"max_session_seconds":  sess.MaxSessionSeconds,
		},
	}
	if sess.ConnectionID != "" {
		resp["connection_id"] = sess.ConnectionID
	}
	if sess.ProvisionStep != "" {
		resp["provision_step"] = sess.ProvisionStep
	}
	if sess.RelayInstanceID != "" {
		resp["instance_id"] = sess.RelayInstanceID
	}
	return resp
}
func provisionedPublicIP(_ string, publicIP string) string { return publicIP }

func generatePairToken(length int) (string, error) {
	const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
	if length <= 0 {
		return "", errors.New("invalid token length")
	}
	max := big.NewInt(int64(len(alphabet)))
	out := make([]byte, length)
	for i := range out {
		idx, err := rand.Int(rand.Reader, max)
		if err != nil {
			return "", err
		}
		out[i] = alphabet[idx.Int64()]
	}
	return string(out), nil
}

func generateRelayWSToken() (string, error) {
	b := make([]byte, 24)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}
