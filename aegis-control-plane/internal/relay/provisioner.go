package relay

import "context"

type ProvisionRequest struct {
	SessionID   string
	UserID      string
	Region      string
	StreamToken string
}

type ProvisionResult struct {
	InstanceID   string
	AMIID        string
	InstanceType string
	PublicIP     string
	SRTPort      int
}

type DeprovisionRequest struct {
	SessionID  string
	UserID     string
	Region     string
	InstanceID string
}

type Provisioner interface {
	Provision(ctx context.Context, req ProvisionRequest) (ProvisionResult, error)
	Deprovision(ctx context.Context, req DeprovisionRequest) error
}

// EIPStore abstracts the persistent storage operations for Elastic IP
// management. The AWSProvisioner uses this to look up and persist per-user
// EIP allocations without depending on the full store package.
type EIPStore interface {
	GetUserEIP(ctx context.Context, userID string) (allocID, ip string, err error)
	SetUserEIP(ctx context.Context, userID, allocID, ip string) error
}