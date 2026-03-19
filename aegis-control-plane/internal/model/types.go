package model

import "time"

type SessionStatus string

const (
	SessionProvisioning SessionStatus = "provisioning"
	SessionActive       SessionStatus = "active"
	SessionGrace        SessionStatus = "grace"
	SessionStopped      SessionStatus = "stopped"

	StepLaunchingInstance  = "launching_instance"
	StepWaitingForInstance = "waiting_for_instance"
	StepStartingDocker     = "starting_docker"
	StepStartingContainers = "starting_containers"
	StepCreatingStream     = "creating_stream"
	StepReady              = "ready"
)

type Session struct {
	ID                 string
	UserID             string
	RelayRecordID      *string
	RelayInstanceID    string
	Status             SessionStatus
	ProvisionStep      string
	Region             string
	PairToken          string
	RelayWSToken       string
	StreamToken        string
	PublicIP           string
	SRTPort            int
	RelayHostname      string
	StartedAt          time.Time
	StoppedAt          *time.Time
	DurationSeconds    int
	GraceWindowSeconds int
	MaxSessionSeconds  int
}

type UsageCurrent struct {
	PlanTier         string
	PlanStatus       string
	CycleStart       time.Time
	CycleEnd         time.Time
	IncludedSeconds  int
	ConsumedSeconds  int
	RemainingSeconds int
	OverageSeconds   int
}

type RelayEntitlement struct {
	PlanTier         string
	PlanStatus       string
	IncludedSeconds  int
	ConsumedSeconds  int
	RemainingSeconds int
	OverageSeconds   int
	Allowed          bool
	ReasonCode       string
}

type User struct {
	ID          string
	Email       string
	DisplayName string
}

type BYORConfig struct {
	Host     string
	Port     int
	StreamID string
}

type AuthSession struct {
	ID               string
	UserID           string
	RefreshTokenHash string
	ClientPlatform   string
	ClientVersion    string
	DeviceName       string
	CreatedAt        time.Time
	LastSeenAt       time.Time
	ExpiresAt        time.Time
	RevokedAt        *time.Time
}

func (s *AuthSession) IsActive(now time.Time) bool {
	if s == nil {
		return false
	}
	if s.RevokedAt != nil {
		return false
	}
	return now.Before(s.ExpiresAt)
}

type PluginLoginAttemptStatus string

const (
	PluginLoginPending   PluginLoginAttemptStatus = "pending"
	PluginLoginCompleted PluginLoginAttemptStatus = "completed"
	PluginLoginDenied    PluginLoginAttemptStatus = "denied"
	PluginLoginExpired   PluginLoginAttemptStatus = "expired"
)

type PluginLoginAttempt struct {
	ID                 string
	PollTokenHash      string
	Status             PluginLoginAttemptStatus
	UserID             *string
	CompletedSessionID *string
	ClientPlatform     string
	ClientVersion      string
	DeviceName         string
	DenyReasonCode     string
	ExpiresAt          time.Time
	CompletedAt        *time.Time
	CreatedAt          time.Time
}

func (a *PluginLoginAttempt) IsExpired(now time.Time) bool {
	if a == nil {
		return false
	}
	return now.After(a.ExpiresAt)
}

type RelayManifestEntry struct {
	Region              string
	AMIID               string
	DefaultInstanceType string
	UpdatedAt           time.Time
}
