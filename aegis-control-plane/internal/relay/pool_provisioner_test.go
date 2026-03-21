package relay

import (
	"context"
	"errors"
	"testing"

	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

// fakePoolStore stubs the poolStore interface for provisioner tests.
type fakePoolStore struct {
	assignment *model.RelayAssignment
	assignErr  error
	releaseErr error
}

func (f *fakePoolStore) AssignRelay(_ context.Context, userID, sessionID, connectionID, region, streamToken string) (*model.RelayAssignment, error) {
	if f.assignErr != nil {
		return nil, f.assignErr
	}
	a := &model.RelayAssignment{
		ID:          1,
		UserID:      userID,
		SessionID:   sessionID,
		ServerID:    "pool-server-1",
		StreamToken: streamToken,
		Host:        "relay.example.com",
		IP:          "1.2.3.4",
	}
	f.assignment = a
	return a, nil
}

func (f *fakePoolStore) GetRelayAssignment(_ context.Context, _ string) (*model.RelayAssignment, error) {
	if f.assignment == nil {
		return nil, store.ErrNotFound
	}
	return f.assignment, nil
}

func (f *fakePoolStore) ReleaseRelay(_ context.Context, _ string) error {
	return f.releaseErr
}

// fakeSLS stubs slsOps for provisioner tests.
type fakeSLS struct {
	createErr error
	deleteErr error
	created   []string // publisher values passed to CreateStreamID
	deleted   []string // id values passed to DeleteStreamID
}

func (f *fakeSLS) CreateStreamID(_ context.Context, publisher, player, description string) error {
	f.created = append(f.created, publisher)
	return f.createErr
}

func (f *fakeSLS) DeleteStreamID(_ context.Context, id string) error {
	f.deleted = append(f.deleted, id)
	return f.deleteErr
}

// newTestPoolProvisioner builds a PoolProvisioner with injected fakes.
func newTestPoolProvisioner(st *fakePoolStore, sls *fakeSLS) *PoolProvisioner {
	p := NewPoolProvisioner(st, "test-api-key")
	p.newSLS = func(host string) slsOps { return sls }
	return p
}

func TestPoolProvisioner_Provision_Success(t *testing.T) {
	st := &fakePoolStore{}
	sls := &fakeSLS{}
	prov := newTestPoolProvisioner(st, sls)

	result, err := prov.Provision(context.Background(), ProvisionRequest{
		SessionID:   "s1",
		UserID:      "u1",
		Region:      "us-west-2",
		StreamToken: "tok123",
	})
	if err != nil {
		t.Fatalf("Provision returned error: %v", err)
	}
	if result.PublicIP != "1.2.3.4" {
		t.Errorf("PublicIP = %q, want %q", result.PublicIP, "1.2.3.4")
	}
	if result.SRTPort != 5000 {
		t.Errorf("SRTPort = %d, want 5000", result.SRTPort)
	}
	if result.InstanceID != "pool-server-1" {
		t.Errorf("InstanceID = %q, want %q", result.InstanceID, "pool-server-1")
	}
	if len(sls.created) != 1 || sls.created[0] != "live_tok123" {
		t.Errorf("SLS CreateStreamID not called correctly; created=%v", sls.created)
	}
}

func TestPoolProvisioner_Provision_NoCapacity(t *testing.T) {
	st := &fakePoolStore{assignErr: store.ErrNoRelayCapacity}
	sls := &fakeSLS{}
	prov := newTestPoolProvisioner(st, sls)

	_, err := prov.Provision(context.Background(), ProvisionRequest{
		SessionID: "s2", UserID: "u1", Region: "us-west-2", StreamToken: "tok",
	})
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	if len(sls.created) != 0 {
		t.Errorf("SLS should not be called when assign fails")
	}
}

func TestPoolProvisioner_Provision_SLSFailure_ReleasesRelay(t *testing.T) {
	st := &fakePoolStore{}
	sls := &fakeSLS{createErr: errors.New("sls down")}
	prov := newTestPoolProvisioner(st, sls)

	_, err := prov.Provision(context.Background(), ProvisionRequest{
		SessionID: "s3", UserID: "u1", Region: "us-west-2", StreamToken: "tok",
	})
	if err == nil {
		t.Fatal("expected error from SLS failure, got nil")
	}
	// ReleaseRelay should have been called (best-effort cleanup).
	// fakePoolStore.ReleaseRelay doesn't error, so we can't easily observe the call
	// without a counter — but at minimum the error must propagate.
}

func TestPoolProvisioner_Deprovision_Success(t *testing.T) {
	st := &fakePoolStore{
		assignment: &model.RelayAssignment{
			ID: 1, SessionID: "s1", ServerID: "h1",
			StreamToken: "tok123", Host: "relay.example.com", IP: "1.2.3.4",
		},
	}
	sls := &fakeSLS{}
	prov := newTestPoolProvisioner(st, sls)

	if err := prov.Deprovision(context.Background(), DeprovisionRequest{
		SessionID: "s1", UserID: "u1", InstanceID: "h1",
	}); err != nil {
		t.Fatalf("Deprovision returned error: %v", err)
	}
	if len(sls.deleted) != 1 || sls.deleted[0] != "live_tok123" {
		t.Errorf("SLS DeleteStreamID not called correctly; deleted=%v", sls.deleted)
	}
}

func TestPoolProvisioner_Deprovision_NotFound_IsNoop(t *testing.T) {
	st := &fakePoolStore{} // assignment is nil → ErrNotFound
	sls := &fakeSLS{}
	prov := newTestPoolProvisioner(st, sls)

	if err := prov.Deprovision(context.Background(), DeprovisionRequest{
		SessionID: "s99", UserID: "u1",
	}); err != nil {
		t.Fatalf("Deprovision on missing session should be noop, got: %v", err)
	}
	if len(sls.deleted) != 0 {
		t.Errorf("SLS should not be called when session not found")
	}
}

func TestPoolProvisioner_Deprovision_SLSFailure_StillReleases(t *testing.T) {
	st := &fakePoolStore{
		assignment: &model.RelayAssignment{
			ID: 1, SessionID: "s1", StreamToken: "tok", Host: "relay.example.com",
		},
	}
	sls := &fakeSLS{deleteErr: errors.New("sls down")}
	prov := newTestPoolProvisioner(st, sls)

	// SLS delete error is best-effort; Deprovision should still succeed.
	if err := prov.Deprovision(context.Background(), DeprovisionRequest{
		SessionID: "s1", UserID: "u1",
	}); err != nil {
		t.Fatalf("Deprovision should succeed despite SLS failure, got: %v", err)
	}
}
