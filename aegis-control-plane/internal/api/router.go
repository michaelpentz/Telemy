package api

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"sync"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/google/uuid"

	"github.com/telemyapp/aegis-control-plane/internal/auth"
	"github.com/telemyapp/aegis-control-plane/internal/config"
	"github.com/telemyapp/aegis-control-plane/internal/dns"
	"github.com/telemyapp/aegis-control-plane/internal/metrics"
	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/relay"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

type Store interface {
	StartOrGetSession(rctx context.Context, in store.StartInput) (*model.Session, bool, error)
	ActivateProvisionedSession(rctx context.Context, in store.ActivateProvisionedSessionInput) (*model.Session, error)
	GetActiveSession(rctx context.Context, userID string) (*model.Session, error)
	GetSessionByID(rctx context.Context, userID, sessionID string) (*model.Session, error)
	StopSession(rctx context.Context, userID, sessionID string) (*model.Session, error)
	UpdateProvisionStep(rctx context.Context, sessionID, step string) error
	FinalActivateSession(ctx context.Context, sessionID string) error
	GetUsageCurrent(rctx context.Context, userID string) (*model.UsageCurrent, error)
	RecordRelayHealth(rctx context.Context, in store.RelayHealthInput) error
	ListRelayManifest(rctx context.Context) ([]model.RelayManifestEntry, error)
	GetUserRelaySlug(ctx context.Context, userID string) (string, error)
	GetUserEIP(ctx context.Context, userID string) (string, string, error)
	SetUserEIP(ctx context.Context, userID, allocationID, publicIP string) error
}

type Server struct {
	cfg         config.Config
	store       Store
	provisioner relay.Provisioner
	dns         *dns.Client
	// probeReady and dwell are injectable for tests; nil uses real implementations.
	probeReady func(ctx context.Context, url string) bool
	dwell      func(ctx context.Context, d time.Duration)
	// appCtx is cancelled on process shutdown; provisioning goroutines derive from it.
	appCtx context.Context
	// wg tracks in-flight provisioning goroutines so Shutdown can wait for them.
	wg sync.WaitGroup
}

func NewRouter(cfg config.Config, st Store, prov relay.Provisioner, dnsClient *dns.Client, opts ...RouterOption) (*Server, http.Handler) {
	s := &Server{cfg: cfg, store: st, provisioner: prov, dns: dnsClient, appCtx: context.Background()}
	for _, o := range opts {
		o(s)
	}
	r := chi.NewRouter()
	r.Use(middleware.RequestID)
	r.Use(middleware.RealIP)
	r.Use(middleware.Recoverer)
	// AWS relay provisioning can exceed tens of seconds during EC2 launch/wait.
	r.Use(middleware.Timeout(3 * time.Minute))
	r.Use(maxBodySize(1 << 20)) // 1 MB request body limit

	r.Get("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok"})
	})
	r.Get("/metrics", metrics.Default().Handler().ServeHTTP)

	r.Route("/api/v1", func(v1 chi.Router) {
		v1.With(auth.Middleware(cfg.JWTSecret)).Group(func(authed chi.Router) {
			authed.Post("/relay/start", s.handleRelayStart)
			authed.Get("/relay/active", s.handleRelayActive)
			authed.Post("/relay/stop", s.handleRelayStop)
			authed.Get("/relay/manifest", s.handleRelayManifest)
			authed.Get("/usage/current", s.handleUsageCurrent)
		})

		v1.With(s.relaySharedAuth).Post("/relay/health", s.handleRelayHealth)
	})

	return s, r
}

// RouterOption configures optional Server fields.
type RouterOption func(*Server)

// WithAppContext sets the application-scoped context for background goroutines.
func WithAppContext(ctx context.Context) RouterOption {
	return func(s *Server) { s.appCtx = ctx }
}

// Shutdown waits for in-flight provisioning goroutines to finish, up to the
// given timeout. Returns true if all goroutines completed, false on timeout.
func (s *Server) Shutdown(timeout time.Duration) bool {
	done := make(chan struct{})
	go func() {
		s.wg.Wait()
		close(done)
	}()
	select {
	case <-done:
		log.Printf("server: all provisioning goroutines finished")
		return true
	case <-time.After(timeout):
		log.Printf("server: timed out waiting for %v for provisioning goroutines", timeout)
		return false
	}
}

func (s *Server) relaySharedAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Relay-Auth") != s.cfg.RelaySharedKey {
			writeAPIError(w, http.StatusUnauthorized, "unauthorized", "invalid relay auth")
			return
		}
		next.ServeHTTP(w, r)
	})
}

type apiError struct {
	Error struct {
		Code      string `json:"code"`
		Message   string `json:"message"`
		RequestID string `json:"request_id,omitempty"`
	} `json:"error"`
}

func writeAPIError(w http.ResponseWriter, status int, code, message string) {
	var payload apiError
	payload.Error.Code = code
	payload.Error.Message = message
	writeJSON(w, status, payload)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

// maxBodySize returns middleware that limits request body size for methods that
// carry a body (POST, PUT, PATCH). Exceeding the limit causes the JSON decoder
// to return an error which handlers translate to 413 Request Entity Too Large.
func maxBodySize(n int64) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.Method {
			case http.MethodPost, http.MethodPut, http.MethodPatch:
				r.Body = http.MaxBytesReader(w, r.Body, n)
			}
			next.ServeHTTP(w, r)
		})
	}
}

func parseIdempotencyKey(h string) (uuid.UUID, error) {
	return uuid.Parse(h)
}
