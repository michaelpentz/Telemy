package relay

import (
	"context"
	"fmt"

	"github.com/telemyapp/aegis-control-plane/internal/model"
)

type BYORConfigReader interface {
	GetBYORConfig(ctx context.Context, userID string) (*model.BYORConfig, error)
}

type BYORProvisioner struct {
	store BYORConfigReader
}

func NewBYORProvisioner(store BYORConfigReader) *BYORProvisioner {
	return &BYORProvisioner{store: store}
}

func (p *BYORProvisioner) Provision(ctx context.Context, req ProvisionRequest) (ProvisionResult, error) {
	cfg, err := p.store.GetBYORConfig(ctx, req.UserID)
	if err != nil {
		return ProvisionResult{}, err
	}
	if cfg == nil || cfg.Host == "" || cfg.StreamID == "" {
		return ProvisionResult{}, fmt.Errorf("byor relay config missing for user %s", req.UserID)
	}
	port := cfg.Port
	if port == 0 {
		port = 5000
	}
	return ProvisionResult{
		InstanceID:   "byor-" + req.UserID,
		AMIID:        "byor",
		InstanceType: "byor",
		PublicIP:     cfg.Host,
		SRTPort:      port,
	}, nil
}

func (p *BYORProvisioner) Deprovision(_ context.Context, _ DeprovisionRequest) error {
	return nil
}
