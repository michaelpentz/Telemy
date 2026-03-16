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
	RelayInstanceID    *string
	RelayAWSInstanceID string
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
	CycleStart       time.Time
	CycleEnd         time.Time
	IncludedSeconds  int
	ConsumedSeconds  int
	RemainingSeconds int
	OverageSeconds   int
}

type RelayManifestEntry struct {
	Region              string
	AMIID               string
	DefaultInstanceType string
	UpdatedAt           time.Time
}
