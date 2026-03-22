package relay

import (
	"context"
	"errors"
	"fmt"
	"net/http"

	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

// poolStore is the subset of store.Store used by PoolProvisioner.
type poolStore interface {
	AssignRelay(ctx context.Context, userID, sessionID, connectionID, region, streamToken string) (*model.RelayAssignment, error)
	GetRelayAssignment(ctx context.Context, sessionID string) (*model.RelayAssignment, error)
	ReleaseRelay(ctx context.Context, sessionID string) error
}

// slsOps is the subset of SLSClient used by PoolProvisioner.
type slsOps interface {
	CreateStreamID(ctx context.Context, publisher, player, description string) error
	DeleteStreamID(ctx context.Context, id string) error
}

// PoolProvisioner assigns sessions to the shared relay pool instead of
// launching ephemeral EC2 instances. Provision is near-instant (<1 second).
type PoolProvisioner struct {
	store     poolStore
	slsAPIKey string
	newSLS    func(host string) slsOps // injectable for tests
}

// NewPoolProvisioner creates a PoolProvisioner that uses the given store and
// SLS API key. Each Provision call creates a per-server SLS client on demand.
func NewPoolProvisioner(st poolStore, slsAPIKey string) *PoolProvisioner {
	p := &PoolProvisioner{store: st, slsAPIKey: slsAPIKey}
	p.newSLS = func(host string) slsOps {
		return NewSLSClient("http://"+host+":8090", slsAPIKey)
	}
	return p
}

func (p *PoolProvisioner) Provision(ctx context.Context, req ProvisionRequest) (ProvisionResult, error) {
	assignment, err := p.store.AssignRelay(ctx, req.UserID, req.SessionID, "", req.Region, req.StreamToken)
	if err != nil {
		if err == store.ErrNoRelayCapacity {
			return ProvisionResult{}, fmt.Errorf("pool: no relay capacity available")
		}
		return ProvisionResult{}, fmt.Errorf("pool: assign relay: %w", err)
	}

	sls := p.newSLS(assignment.Host)
	if err := sls.CreateStreamID(ctx, "live_"+req.StreamToken, "play_"+req.StreamToken, ""); err != nil {
		// 409 Conflict means the stream ID already exists — treat as idempotent success.
		var slsErr *SLSError
		if !(errors.As(err, &slsErr) && slsErr.Code == http.StatusConflict) {
			// Best-effort release on SLS failure; server stays in pool.
			_ = p.store.ReleaseRelay(ctx, req.SessionID)
			return ProvisionResult{}, fmt.Errorf("pool: create stream id: %w", err)
		}
	}

	return ProvisionResult{
		InstanceID: assignment.ServerID, // passed back to Deprovision via DeprovisionRequest.InstanceID
		PublicIP:   assignment.IP,
		SRTPort:    5000,
	}, nil
}

func (p *PoolProvisioner) Deprovision(ctx context.Context, req DeprovisionRequest) error {
	assignment, err := p.store.GetRelayAssignment(ctx, req.SessionID)
	if err != nil {
		if err == store.ErrNotFound {
			return nil // already released
		}
		return fmt.Errorf("pool: get assignment: %w", err)
	}

	// Best-effort: remove stream ID from relay. Don't block release on failure.
	sls := p.newSLS(assignment.Host)
	_ = sls.DeleteStreamID(ctx, "play_"+assignment.StreamToken)

	return p.store.ReleaseRelay(ctx, req.SessionID)
}
