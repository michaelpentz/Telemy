package jobs

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/telemyapp/aegis-control-plane/internal/metrics"
)

// stubStore implements Store for testing.
type stubStore struct {
	cleanupErr   error
	rollupErr    error
	reconcileErr error
	upsertErr    error
}

func (s *stubStore) CleanupExpiredIdempotencyRecords(_ context.Context) error {
	return s.cleanupErr
}

func (s *stubStore) RollupLiveSessionDurations(_ context.Context) error {
	return s.rollupErr
}

func (s *stubStore) ReconcileOutageFromHealth(_ context.Context) error {
	return s.reconcileErr
}

func (s *stubStore) UpsertUsageRollups(_ context.Context) error {
	return s.upsertErr
}

func TestRunOnce_Success(t *testing.T) {
	metrics.ResetDefaultForTest()
	r := NewRunner(&stubStore{})

	r.runOnce(context.Background(), "test_job", func(_ context.Context) error { return nil })

	out := metrics.Default().Render()
	if !strings.Contains(out, `aegis_job_runs_total{job="test_job",status="ok"} 1`) {
		t.Fatalf("expected ok counter in output:\n%s", out)
	}
	if !strings.Contains(out, `aegis_job_duration_ms_count{job="test_job"} 1`) {
		t.Fatalf("expected histogram count in output:\n%s", out)
	}
}

func TestRunOnce_Error(t *testing.T) {
	metrics.ResetDefaultForTest()
	r := NewRunner(&stubStore{})

	r.runOnce(context.Background(), "test_job", func(_ context.Context) error {
		return errors.New("db down")
	})

	out := metrics.Default().Render()
	if !strings.Contains(out, `aegis_job_runs_total{job="test_job",status="error"} 1`) {
		t.Fatalf("expected error counter in output:\n%s", out)
	}
	if !strings.Contains(out, `aegis_job_duration_ms_count{job="test_job"} 1`) {
		t.Fatalf("expected histogram count in output:\n%s", out)
	}
}

func TestRunOnce_ErrorDoesNotRecordOk(t *testing.T) {
	metrics.ResetDefaultForTest()
	r := NewRunner(&stubStore{})

	r.runOnce(context.Background(), "test_job", func(_ context.Context) error {
		return errors.New("fail")
	})

	out := metrics.Default().Render()
	if strings.Contains(out, `status="ok"`) {
		t.Fatalf("unexpected ok counter when fn returns error:\n%s", out)
	}
}

func TestRunEvery_RunsImmediately(t *testing.T) {
	metrics.ResetDefaultForTest()
	r := NewRunner(&stubStore{})

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	called := make(chan struct{}, 1)
	fn := func(_ context.Context) error {
		select {
		case called <- struct{}{}:
		default:
		}
		return nil
	}

	go r.runEvery(ctx, "test_job", 10*time.Second, fn)

	select {
	case <-called:
		// success — ran immediately before first tick
	case <-time.After(500 * time.Millisecond):
		t.Fatal("runEvery did not invoke fn immediately on start")
	}
}

func TestRunEvery_FiresOnTicker(t *testing.T) {
	metrics.ResetDefaultForTest()
	r := NewRunner(&stubStore{})

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	count := make(chan struct{}, 10)
	fn := func(_ context.Context) error {
		count <- struct{}{}
		return nil
	}

	go r.runEvery(ctx, "test_job", 20*time.Millisecond, fn)

	// Expect at least 3 invocations (immediate + 2 ticks) within 300ms.
	received := 0
	deadline := time.After(300 * time.Millisecond)
	for received < 3 {
		select {
		case <-count:
			received++
		case <-deadline:
			t.Fatalf("only received %d invocations, expected at least 3", received)
		}
	}
}

func TestRunEvery_CancelsOnContextDone(t *testing.T) {
	metrics.ResetDefaultForTest()
	r := NewRunner(&stubStore{})

	ctx, cancel := context.WithCancel(context.Background())

	done := make(chan struct{})
	go func() {
		defer close(done)
		r.runEvery(ctx, "test_job", 50*time.Millisecond, func(_ context.Context) error { return nil })
	}()

	// Let at least one tick fire.
	time.Sleep(20 * time.Millisecond)
	cancel()

	select {
	case <-done:
		// success
	case <-time.After(2 * time.Second):
		t.Fatal("runEvery goroutine did not exit after context cancellation")
	}
}

func TestStart_GracefulShutdown(t *testing.T) {
	metrics.ResetDefaultForTest()
	r := NewRunner(&stubStore{})

	ctx, cancel := context.WithCancel(context.Background())
	r.Start(ctx)

	// Give goroutines time to start and fire their initial runs.
	time.Sleep(20 * time.Millisecond)
	cancel()

	// Allow goroutines to drain — no explicit join, but no panic/deadlock expected.
	time.Sleep(20 * time.Millisecond)
}
