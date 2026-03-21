package store

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	"github.com/telemyapp/aegis-control-plane/internal/model"
)

// Session timer defaults (RF-031).
const (
	DefaultMaxSessionSeconds  = 57600 // 16 hours
	DefaultGraceWindowSeconds = 600   // 10 minutes
)

var (
	ErrNotFound            = errors.New("not found")
	ErrIdempotencyMismatch = errors.New("idempotency mismatch")
	ErrRelayHealthRejected = errors.New("relay health rejected")
	ErrConflict            = errors.New("conflict")
	ErrNoRelayCapacity     = errors.New("no relay capacity available")
)

type Store struct {
	db          DB
	relayDomain string
}

type DB interface {
	QueryRow(ctx context.Context, sql string, args ...any) pgx.Row
	Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error)
	Exec(ctx context.Context, sql string, arguments ...any) (pgconn.CommandTag, error)
	BeginTx(ctx context.Context, txOptions pgx.TxOptions) (pgx.Tx, error)
}

type StartInput struct {
	UserID         string
	ConnectionID   string
	Region         string
	RequestedBy    string
	IdempotencyKey uuid.UUID
	RequestHash    string
}

type RelayHealthInput struct {
	SessionID            string
	InstanceID           string
	ObservedAt           time.Time
	IngestActive         bool
	EgressActive         bool
	SessionUptimeSeconds int
	RawPayload           json.RawMessage
}

type ActivateProvisionedSessionInput struct {
	UserID       string
	SessionID    string
	Region       string
	InstanceID   string
	AMIID        string
	InstanceType string
	PublicIP     string
	SRTPort      int
	PairToken    string
	RelayWSToken string
}

type CreateAuthSessionInput struct {
	ID               string
	UserID           string
	RefreshTokenHash string
	ClientPlatform   string
	ClientVersion    string
	DeviceName       string
	ExpiresAt        time.Time
}

type CreatePluginLoginAttemptInput struct {
	ID             string
	PollTokenHash  string
	ClientPlatform string
	ClientVersion  string
	DeviceName     string
	ExpiresAt      time.Time
}

type FinalizePluginLoginAttemptInput struct {
	AttemptID      string
	Status         model.PluginLoginAttemptStatus
	UserID         string
	DenyReasonCode string
}

func New(db DB, relayDomain ...string) *Store {
	domain := "relay.telemyapp.com"
	if len(relayDomain) > 0 && relayDomain[0] != "" {
		domain = relayDomain[0]
	}
	return &Store{db: db, relayDomain: domain}
}

func (s *Store) DB() DB {
	return s.db
}

func HashJSON(v any) (string, error) {
	b, err := json.Marshal(v)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:]), nil
}

func (s *Store) GetActiveSession(ctx context.Context, userID string) (*model.Session, error) {
	const q = `
select s.id, s.user_id, coalesce(s.connection_id, ''), coalesce(s.relay_instance_id, ''), coalesce(ri.instance_id, ''), s.status, coalesce(s.provision_step, ''), s.region, s.pair_token, s.relay_ws_token, u.stream_token,
       coalesce(host(ri.public_ip), ''), coalesce(ri.srt_port, 5000), coalesce(ri.ws_url, ''),
       s.started_at, s.stopped_at, s.duration_seconds, s.grace_window_seconds, s.max_session_seconds,
       coalesce(u.relay_slug, '')
from sessions s
left join relay_instances ri on ri.id = s.relay_instance_id
left join users u on u.id = s.user_id
where s.user_id = $1 and s.status in ('provisioning', 'active', 'grace')
order by s.created_at desc
limit 1`

	var out model.Session
	var relayInstanceID string
	var relaySlug string
	var stoppedAt *time.Time
	var wsURLIgnored string
	if err := s.db.QueryRow(ctx, q, userID).Scan(
		&out.ID, &out.UserID, &out.ConnectionID, &relayInstanceID, &out.RelayInstanceID, &out.Status, &out.ProvisionStep, &out.Region, &out.PairToken, &out.RelayWSToken, &out.StreamToken,
		&out.PublicIP, &out.SRTPort, &wsURLIgnored,
		&out.StartedAt, &stoppedAt, &out.DurationSeconds, &out.GraceWindowSeconds, &out.MaxSessionSeconds,
		&relaySlug,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, nil
		}
		return nil, err
	}
	out.StoppedAt = stoppedAt
	out.RelayRecordID = strPtr(relayInstanceID)
	if relaySlug != "" && out.PublicIP != "" {
		out.RelayHostname = relaySlug + "." + s.relayDomain
	}
	return &out, nil
}

func (s *Store) GetActiveSessionByConnection(ctx context.Context, userID, connectionID string) (*model.Session, error) {
	const q = `
select s.id, s.user_id, coalesce(s.connection_id, ''), coalesce(s.relay_instance_id, ''), coalesce(ri.instance_id, ''), s.status, coalesce(s.provision_step, ''), s.region, s.pair_token, s.relay_ws_token, u.stream_token,
       coalesce(host(ri.public_ip), ''), coalesce(ri.srt_port, 5000), coalesce(ri.ws_url, ''),
       s.started_at, s.stopped_at, s.duration_seconds, s.grace_window_seconds, s.max_session_seconds,
       coalesce(u.relay_slug, '')
from sessions s
left join relay_instances ri on ri.id = s.relay_instance_id
left join users u on u.id = s.user_id
where s.user_id = $1 and s.connection_id = $2 and s.status in ('provisioning', 'active', 'grace')
order by s.created_at desc
limit 1`

	var out model.Session
	var relayInstanceID string
	var relaySlug string
	var stoppedAt *time.Time
	var wsURLIgnored string
	if err := s.db.QueryRow(ctx, q, userID, connectionID).Scan(
		&out.ID, &out.UserID, &out.ConnectionID, &relayInstanceID, &out.RelayInstanceID, &out.Status, &out.ProvisionStep, &out.Region, &out.PairToken, &out.RelayWSToken, &out.StreamToken,
		&out.PublicIP, &out.SRTPort, &wsURLIgnored,
		&out.StartedAt, &stoppedAt, &out.DurationSeconds, &out.GraceWindowSeconds, &out.MaxSessionSeconds,
		&relaySlug,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, nil
		}
		return nil, err
	}
	out.StoppedAt = stoppedAt
	out.RelayRecordID = strPtr(relayInstanceID)
	if relaySlug != "" && out.PublicIP != "" {
		out.RelayHostname = relaySlug + "." + s.relayDomain
	}
	return &out, nil
}

func (s *Store) ListActiveSessions(ctx context.Context, userID string) ([]model.Session, error) {
	const q = `
select s.id, s.user_id, coalesce(s.connection_id, ''), coalesce(s.relay_instance_id, ''), coalesce(ri.instance_id, ''), s.status, coalesce(s.provision_step, ''), s.region, s.pair_token, s.relay_ws_token, u.stream_token,
       coalesce(host(ri.public_ip), ''), coalesce(ri.srt_port, 5000), coalesce(ri.ws_url, ''),
       s.started_at, s.stopped_at, s.duration_seconds, s.grace_window_seconds, s.max_session_seconds,
       coalesce(u.relay_slug, '')
from sessions s
left join relay_instances ri on ri.id = s.relay_instance_id
left join users u on u.id = s.user_id
where s.user_id = $1 and s.status in ('provisioning', 'active', 'grace')
order by s.created_at desc`

	rows, err := s.db.Query(ctx, q, userID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := make([]model.Session, 0)
	for rows.Next() {
		var sess model.Session
		var relayInstanceID string
		var relaySlug string
		var stoppedAt *time.Time
		var wsURLIgnored string
		if err := rows.Scan(
			&sess.ID, &sess.UserID, &sess.ConnectionID, &relayInstanceID, &sess.RelayInstanceID, &sess.Status, &sess.ProvisionStep, &sess.Region, &sess.PairToken, &sess.RelayWSToken, &sess.StreamToken,
			&sess.PublicIP, &sess.SRTPort, &wsURLIgnored,
			&sess.StartedAt, &stoppedAt, &sess.DurationSeconds, &sess.GraceWindowSeconds, &sess.MaxSessionSeconds,
			&relaySlug,
		); err != nil {
			return nil, err
		}
		sess.StoppedAt = stoppedAt
		sess.RelayRecordID = strPtr(relayInstanceID)
		if relaySlug != "" && sess.PublicIP != "" {
			sess.RelayHostname = relaySlug + "." + s.relayDomain
		}
		out = append(out, sess)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return out, nil
}

func (s *Store) StartOrGetSession(ctx context.Context, in StartInput) (*model.Session, bool, error) {
	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return nil, false, err
	}
	defer tx.Rollback(ctx)

	var storedHash, storedSessionID string
	const idemLookup = `
select request_hash, session_id
from idempotency_records
where user_id = $1 and endpoint = '/api/v1/relay/start' and idempotency_key = $2 and expires_at > now()`
	err = tx.QueryRow(ctx, idemLookup, in.UserID, in.IdempotencyKey).Scan(&storedHash, &storedSessionID)
	if err == nil {
		if storedHash != in.RequestHash {
			return nil, false, ErrIdempotencyMismatch
		}
		// Live lookup so replay always reflects current session state, not stale pre-activation snapshot.
		sess, err := s.getSessionByIDTx(ctx, tx, in.UserID, storedSessionID)
		if err != nil {
			return nil, false, err
		}
		if err := tx.Commit(ctx); err != nil {
			return nil, false, err
		}
		return sess, false, nil
	}
	if err != nil && !errors.Is(err, pgx.ErrNoRows) {
		return nil, false, err
	}

	existing, err := s.getActiveSessionTx(ctx, tx, in.UserID, in.ConnectionID)
	if err != nil {
		return nil, false, err
	}
	if existing != nil {
		if err := s.persistIdempotencyRecord(ctx, tx, in, existing); err != nil {
			return nil, false, err
		}
		if err := tx.Commit(ctx); err != nil {
			return nil, false, err
		}
		return existing, false, nil
	}

	newID := "ses_" + uuid.NewString()
	now := time.Now().UTC()
const insertSession = `
insert into sessions
  (id, user_id, connection_id, status, region, idempotency_key, requested_by, pair_token, relay_ws_token, started_at, max_session_seconds, grace_window_seconds, duration_seconds, reconciled_seconds, created_at, updated_at)
values
  ($1, $2, nullif($3, ''), 'provisioning', $4, $5, $6, '', '', $7, $8, $9, 0, 0, $7, $7)`
	if _, err := tx.Exec(ctx, insertSession, newID, in.UserID, in.ConnectionID, in.Region, in.IdempotencyKey, in.RequestedBy, now, DefaultMaxSessionSeconds, DefaultGraceWindowSeconds); err != nil {
		return nil, false, err
	}

	sess, err := s.getSessionByIDTx(ctx, tx, in.UserID, newID)
	if err != nil {
		return nil, false, err
	}

	if err := s.persistIdempotencyRecord(ctx, tx, in, sess); err != nil {
		return nil, false, err
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, false, err
	}
	return sess, true, nil
}

func (s *Store) getActiveSessionTx(ctx context.Context, tx pgx.Tx, userID, connectionID string) (*model.Session, error) {
	const q = `
select s.id, s.user_id, coalesce(s.connection_id, ''), coalesce(s.relay_instance_id, ''), coalesce(ri.instance_id, ''), s.status, coalesce(s.provision_step, ''), s.region, s.pair_token, s.relay_ws_token, u.stream_token,
       coalesce(host(ri.public_ip), ''), coalesce(ri.srt_port, 5000), coalesce(ri.ws_url, ''),
       s.started_at, s.stopped_at, s.duration_seconds, s.grace_window_seconds, s.max_session_seconds,
       coalesce(u.relay_slug, '')
from sessions s
left join relay_instances ri on ri.id = s.relay_instance_id
left join users u on u.id = s.user_id
where s.user_id = $1
  and (($2 = '' and s.connection_id is null) or s.connection_id = nullif($2, ''))
  and s.status in ('provisioning', 'active', 'grace')
order by s.created_at desc
limit 1
for update of s`
	var out model.Session
	var relayInstanceID string
	var relaySlug string
	var stoppedAt *time.Time
	var wsURLIgnored string
	if err := tx.QueryRow(ctx, q, userID, connectionID).Scan(
		&out.ID, &out.UserID, &out.ConnectionID, &relayInstanceID, &out.RelayInstanceID, &out.Status, &out.ProvisionStep, &out.Region, &out.PairToken, &out.RelayWSToken, &out.StreamToken,
		&out.PublicIP, &out.SRTPort, &wsURLIgnored,
		&out.StartedAt, &stoppedAt, &out.DurationSeconds, &out.GraceWindowSeconds, &out.MaxSessionSeconds,
		&relaySlug,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, nil
		}
		return nil, err
	}
	out.StoppedAt = stoppedAt
	out.RelayRecordID = strPtr(relayInstanceID)
	if relaySlug != "" && out.PublicIP != "" {
		out.RelayHostname = relaySlug + "." + s.relayDomain
	}
	return &out, nil
}

func (s *Store) ActivateProvisionedSession(ctx context.Context, in ActivateProvisionedSessionInput) (*model.Session, error) {
	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)

	relayID := "rly_" + uuid.NewString()
	now := time.Now().UTC()
	const insertRelay = `
insert into relay_instances
  (id, session_id, instance_id, region, ami_id, instance_type, public_ip, srt_port, ws_url, state, launched_at, created_at)
values
  ($1, $2, $3, $4, $5, $6, nullif($7, '')::inet, $8, '', 'running', $9, $9)`
	if _, err := tx.Exec(ctx, insertRelay,
		relayID, in.SessionID, in.InstanceID, in.Region, in.AMIID, in.InstanceType, in.PublicIP, in.SRTPort, now,
	); err != nil {
		return nil, err
	}

	const updateSession = `
update sessions
set relay_instance_id = $3,
    pair_token = $4,
    relay_ws_token = $5,
    updated_at = now()
where user_id = $1 and id = $2 and status = 'provisioning'`
	tag, err := tx.Exec(ctx, updateSession, in.UserID, in.SessionID, relayID, in.PairToken, in.RelayWSToken)
	if err != nil {
		return nil, err
	}
	if tag.RowsAffected() == 0 {
		return nil, ErrNotFound
	}

	sess, err := s.getSessionByIDTx(ctx, tx, in.UserID, in.SessionID)
	if err != nil {
		return nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, err
	}
	return sess, nil
}

func (s *Store) getSessionByIDTx(ctx context.Context, tx pgx.Tx, userID, sessionID string) (*model.Session, error) {
	const q = `
select s.id, s.user_id, coalesce(s.connection_id, ''), coalesce(s.relay_instance_id, ''), coalesce(ri.instance_id, ''), s.status, coalesce(s.provision_step, ''), s.region, s.pair_token, s.relay_ws_token, u.stream_token,
       coalesce(host(ri.public_ip), ''), coalesce(ri.srt_port, 5000), coalesce(ri.ws_url, ''),
       s.started_at, s.stopped_at, s.duration_seconds, s.grace_window_seconds, s.max_session_seconds,
       coalesce(u.relay_slug, '')
from sessions s
left join relay_instances ri on ri.id = s.relay_instance_id
left join users u on u.id = s.user_id
where s.user_id = $1 and s.id = $2
limit 1`
	var out model.Session
	var relayInstanceID string
	var relaySlug string
	var stoppedAt *time.Time
	var wsURLIgnored string
	if err := tx.QueryRow(ctx, q, userID, sessionID).Scan(
		&out.ID, &out.UserID, &out.ConnectionID, &relayInstanceID, &out.RelayInstanceID, &out.Status, &out.ProvisionStep, &out.Region, &out.PairToken, &out.RelayWSToken, &out.StreamToken,
		&out.PublicIP, &out.SRTPort, &wsURLIgnored,
		&out.StartedAt, &stoppedAt, &out.DurationSeconds, &out.GraceWindowSeconds, &out.MaxSessionSeconds,
		&relaySlug,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	out.StoppedAt = stoppedAt
	out.RelayRecordID = strPtr(relayInstanceID)
	if relaySlug != "" && out.PublicIP != "" {
		out.RelayHostname = relaySlug + "." + s.relayDomain
	}
	return &out, nil
}

func (s *Store) GetSessionByID(ctx context.Context, userID, sessionID string) (*model.Session, error) {
	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)
	sess, err := s.getSessionByIDTx(ctx, tx, userID, sessionID)
	if err != nil {
		return nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, err
	}
	return sess, nil
}

func (s *Store) persistIdempotencyRecord(ctx context.Context, tx pgx.Tx, in StartInput, sess *model.Session) error {
	resp, err := json.Marshal(sess)
	if err != nil {
		return err
	}
	const q = `
insert into idempotency_records
  (user_id, endpoint, idempotency_key, request_hash, response_json, session_id, created_at, expires_at)
values
  ($1, '/api/v1/relay/start', $2, $3, $4, $5, now(), now() + interval '1 hour')
on conflict (user_id, endpoint, idempotency_key)
do update set response_json = excluded.response_json, session_id = excluded.session_id, expires_at = now() + interval '1 hour'`
	_, err = tx.Exec(ctx, q, in.UserID, in.IdempotencyKey, in.RequestHash, resp, sess.ID)
	return err
}

func (s *Store) StopSession(ctx context.Context, userID, sessionID string) (*model.Session, error) {
	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)

	curr, err := s.getSessionByIDTx(ctx, tx, userID, sessionID)
	if err != nil {
		return nil, err
	}
	if curr.Status != model.SessionStopped {
		const stopQ = `
update sessions
set status = 'stopped', stopped_at = now(), updated_at = now()
where user_id = $1 and id = $2 and status in ('provisioning', 'active', 'grace')`
		tag, err := tx.Exec(ctx, stopQ, userID, sessionID)
		if err != nil {
			return nil, err
		}
		if tag.RowsAffected() == 0 {
			return nil, ErrNotFound
		}
		if curr.RelayRecordID != nil {
			const relayQ = `
update relay_instances
set state = 'terminated', terminated_at = coalesce(terminated_at, now())
where id = $1`
			if _, err := tx.Exec(ctx, relayQ, *curr.RelayRecordID); err != nil {
				return nil, err
			}
		}
	}

	out, err := s.getSessionByIDTx(ctx, tx, userID, sessionID)
	if err != nil {
		return nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, err
	}
	return out, nil
}

func (s *Store) GetUsageCurrent(ctx context.Context, userID string) (*model.UsageCurrent, error) {
	const q = `
select
  u.plan_tier,
  u.plan_status,
  u.cycle_start_at,
  u.cycle_end_at,
  u.included_seconds,
  coalesce(sum(ur.billable_seconds), 0) as consumed_seconds
from users u
left join usage_records ur
  on ur.user_id = u.id
 and ur.cycle_start_at = u.cycle_start_at
 and ur.cycle_end_at = u.cycle_end_at
where u.id = $1
group by u.plan_tier, u.plan_status, u.cycle_start_at, u.cycle_end_at, u.included_seconds`
	var out model.UsageCurrent
	if err := s.db.QueryRow(ctx, q, userID).Scan(
		&out.PlanTier, &out.PlanStatus, &out.CycleStart, &out.CycleEnd, &out.IncludedSeconds, &out.ConsumedSeconds,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	out.RemainingSeconds = max(out.IncludedSeconds-out.ConsumedSeconds, 0)
	out.OverageSeconds = max(out.ConsumedSeconds-out.IncludedSeconds, 0)
	return &out, nil
}

func (s *Store) GetUser(ctx context.Context, userID string) (*model.User, error) {
	var out model.User
	err := s.db.QueryRow(ctx, `select id, email, coalesce(display_name, '') from users where id = $1`, userID).
		Scan(&out.ID, &out.Email, &out.DisplayName)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	return &out, nil
}

func (s *Store) RegenerateStreamToken(ctx context.Context, userID string) (string, error) {
	const maxAttempts = 5
	const q = `update users set stream_token = $2 where id = $1 returning stream_token`

	for range maxAttempts {
		streamToken, err := generateStreamToken()
		if err != nil {
			return "", err
		}

		var updatedToken string
		err = s.db.QueryRow(ctx, q, userID, streamToken).Scan(&updatedToken)
		if err == nil {
			return updatedToken, nil
		}
		if errors.Is(err, pgx.ErrNoRows) {
			return "", ErrNotFound
		}

		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) && pgErr.Code == "23505" {
			continue
		}
		return "", err
	}

	return "", ErrConflict
}

func (s *Store) GetRelayEntitlement(ctx context.Context, userID string) (*model.RelayEntitlement, error) {
	usage, err := s.GetUsageCurrent(ctx, userID)
	if err != nil {
		return nil, err
	}

	var activeManagedConns int
	if err := s.db.QueryRow(ctx, `
select count(*)
from sessions
where user_id = $1
  and status in ('provisioning', 'active', 'grace')`, userID).Scan(&activeManagedConns); err != nil {
		return nil, err
	}

	out := &model.RelayEntitlement{
		PlanTier:           usage.PlanTier,
		PlanStatus:         usage.PlanStatus,
		ActiveManagedConns: activeManagedConns,
		IncludedSeconds:    usage.IncludedSeconds,
		ConsumedSeconds:    usage.ConsumedSeconds,
		RemainingSeconds:   usage.RemainingSeconds,
		OverageSeconds:     usage.OverageSeconds,
	}

	switch usage.PlanStatus {
	case "active", "trial":
	default:
		out.RelayAccessStatus = "disabled"
		out.ReasonCode = "subscription_inactive"
		return out, nil
	}

	switch usage.PlanTier {
	case "starter", "standard":
		out.MaxConcurrentConns = 1
	case "pro":
		out.MaxConcurrentConns = 3
	case "":
		out.RelayAccessStatus = "subscription_required"
		out.ReasonCode = "subscription_required"
		return out, nil
	default:
		out.RelayAccessStatus = "subscription_required"
		out.ReasonCode = "subscription_required"
		return out, nil
	}

	if out.ActiveManagedConns >= out.MaxConcurrentConns {
		out.RelayAccessStatus = "disabled"
		out.ReasonCode = "connection_limit_reached"
		return out, nil
	}

	out.Allowed = out.MaxConcurrentConns > 0
	if out.Allowed {
		out.RelayAccessStatus = "enabled"
		return out, nil
	}

	out.RelayAccessStatus = "disabled"
	return out, nil
}

func (s *Store) CreateAuthSession(ctx context.Context, in CreateAuthSessionInput) (*model.AuthSession, error) {
	const q = `
insert into auth_sessions
  (id, user_id, refresh_token_hash, client_platform, client_version, device_name, created_at, last_seen_at, expires_at)
values
  ($1, $2, $3, $4, $5, $6, now(), now(), $7)`
	if _, err := s.db.Exec(ctx, q, in.ID, in.UserID, in.RefreshTokenHash, in.ClientPlatform, in.ClientVersion, in.DeviceName, in.ExpiresAt); err != nil {
		return nil, err
	}
	return s.GetAuthSession(ctx, in.ID)
}

func (s *Store) GetAuthSession(ctx context.Context, sessionID string) (*model.AuthSession, error) {
	const q = `
select id, user_id, refresh_token_hash, client_platform, coalesce(client_version, ''), coalesce(device_name, ''), created_at, last_seen_at, expires_at, revoked_at
from auth_sessions
where id = $1`
	var out model.AuthSession
	if err := s.db.QueryRow(ctx, q, sessionID).Scan(
		&out.ID, &out.UserID, &out.RefreshTokenHash, &out.ClientPlatform, &out.ClientVersion, &out.DeviceName,
		&out.CreatedAt, &out.LastSeenAt, &out.ExpiresAt, &out.RevokedAt,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	return &out, nil
}

func (s *Store) GetAuthSessionByRefreshHash(ctx context.Context, refreshTokenHash string) (*model.AuthSession, error) {
	const q = `
select id, user_id, refresh_token_hash, client_platform, coalesce(client_version, ''), coalesce(device_name, ''), created_at, last_seen_at, expires_at, revoked_at
from auth_sessions
where refresh_token_hash = $1`
	var out model.AuthSession
	if err := s.db.QueryRow(ctx, q, refreshTokenHash).Scan(
		&out.ID, &out.UserID, &out.RefreshTokenHash, &out.ClientPlatform, &out.ClientVersion, &out.DeviceName,
		&out.CreatedAt, &out.LastSeenAt, &out.ExpiresAt, &out.RevokedAt,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	return &out, nil
}

func (s *Store) RotateAuthSession(ctx context.Context, sessionID, refreshTokenHash string, expiresAt time.Time) (*model.AuthSession, error) {
	const q = `
update auth_sessions
set refresh_token_hash = $2,
    expires_at = $3,
    last_seen_at = now()
where id = $1
  and revoked_at is null
  and expires_at > now()`
	tag, err := s.db.Exec(ctx, q, sessionID, refreshTokenHash, expiresAt)
	if err != nil {
		return nil, err
	}
	if tag.RowsAffected() == 0 {
		return nil, ErrNotFound
	}
	return s.GetAuthSession(ctx, sessionID)
}

func (s *Store) RevokeAuthSession(ctx context.Context, sessionID string) error {
	tag, err := s.db.Exec(ctx, `update auth_sessions set revoked_at = coalesce(revoked_at, now()), last_seen_at = now() where id = $1`, sessionID)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) CreatePluginLoginAttempt(ctx context.Context, in CreatePluginLoginAttemptInput) (*model.PluginLoginAttempt, error) {
	const q = `
insert into plugin_login_attempts
  (id, poll_token_hash, status, client_platform, client_version, device_name, expires_at, created_at)
values
  ($1, $2, 'pending', $3, $4, $5, $6, now())`
	if _, err := s.db.Exec(ctx, q, in.ID, in.PollTokenHash, in.ClientPlatform, in.ClientVersion, in.DeviceName, in.ExpiresAt); err != nil {
		return nil, err
	}
	return s.GetPluginLoginAttempt(ctx, in.ID)
}

func (s *Store) GetPluginLoginAttempt(ctx context.Context, attemptID string) (*model.PluginLoginAttempt, error) {
	const q = `
select id, poll_token_hash, status, user_id, completed_session_id, client_platform, coalesce(client_version, ''), coalesce(device_name, ''),
       coalesce(deny_reason_code, ''), expires_at, completed_at, created_at
from plugin_login_attempts
where id = $1`
	var out model.PluginLoginAttempt
	if err := s.db.QueryRow(ctx, q, attemptID).Scan(
		&out.ID, &out.PollTokenHash, &out.Status, &out.UserID, &out.CompletedSessionID, &out.ClientPlatform, &out.ClientVersion, &out.DeviceName,
		&out.DenyReasonCode, &out.ExpiresAt, &out.CompletedAt, &out.CreatedAt,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	return &out, nil
}

func (s *Store) GetPluginLoginAttemptByPollToken(ctx context.Context, attemptID, pollTokenHash string) (*model.PluginLoginAttempt, error) {
	const q = `
select id, poll_token_hash, status, user_id, completed_session_id, client_platform, coalesce(client_version, ''), coalesce(device_name, ''),
       coalesce(deny_reason_code, ''), expires_at, completed_at, created_at
from plugin_login_attempts
where id = $1 and poll_token_hash = $2`
	var out model.PluginLoginAttempt
	if err := s.db.QueryRow(ctx, q, attemptID, pollTokenHash).Scan(
		&out.ID, &out.PollTokenHash, &out.Status, &out.UserID, &out.CompletedSessionID, &out.ClientPlatform, &out.ClientVersion, &out.DeviceName,
		&out.DenyReasonCode, &out.ExpiresAt, &out.CompletedAt, &out.CreatedAt,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	return &out, nil
}

func (s *Store) FinalizePluginLoginAttempt(ctx context.Context, in FinalizePluginLoginAttemptInput) error {
	if in.Status != model.PluginLoginCompleted && in.Status != model.PluginLoginDenied {
		return fmt.Errorf("invalid plugin login attempt status: %s", in.Status)
	}
	const q = `
update plugin_login_attempts
set status = $2,
    user_id = case when $2 = 'completed' then $3 else null end,
    deny_reason_code = case when $2 = 'denied' then $4 else null end,
    completed_at = now()
where id = $1
  and status = 'pending'
  and expires_at > now()`
	tag, err := s.db.Exec(ctx, q, in.AttemptID, in.Status, nullableString(in.UserID), nullableString(in.DenyReasonCode))
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) MarkPluginLoginAttemptExpired(ctx context.Context, attemptID string) error {
	tag, err := s.db.Exec(ctx, `update plugin_login_attempts set status = 'expired' where id = $1 and status = 'pending'`, attemptID)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) ClaimCompletedPluginLoginAttempt(ctx context.Context, attemptID, pollTokenHash string, authIn CreateAuthSessionInput) (*model.PluginLoginAttempt, *model.AuthSession, error) {
	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return nil, nil, err
	}
	defer tx.Rollback(ctx)

	const selectQ = `
select id, poll_token_hash, status, user_id, completed_session_id, client_platform, coalesce(client_version, ''), coalesce(device_name, ''),
       coalesce(deny_reason_code, ''), expires_at, completed_at, created_at
from plugin_login_attempts
where id = $1 and poll_token_hash = $2
for update`
	var attempt model.PluginLoginAttempt
	if err := tx.QueryRow(ctx, selectQ, attemptID, pollTokenHash).Scan(
		&attempt.ID, &attempt.PollTokenHash, &attempt.Status, &attempt.UserID, &attempt.CompletedSessionID, &attempt.ClientPlatform, &attempt.ClientVersion, &attempt.DeviceName,
		&attempt.DenyReasonCode, &attempt.ExpiresAt, &attempt.CompletedAt, &attempt.CreatedAt,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, nil, ErrNotFound
		}
		return nil, nil, err
	}
	if attempt.CompletedSessionID != nil {
		return &attempt, nil, ErrConflict
	}
	if attempt.Status != model.PluginLoginCompleted || attempt.UserID == nil {
		return &attempt, nil, ErrConflict
	}

	const insertSession = `
insert into auth_sessions
  (id, user_id, refresh_token_hash, client_platform, client_version, device_name, created_at, last_seen_at, expires_at)
values
  ($1, $2, $3, $4, $5, $6, now(), now(), $7)`
	if _, err := tx.Exec(ctx, insertSession, authIn.ID, authIn.UserID, authIn.RefreshTokenHash, authIn.ClientPlatform, authIn.ClientVersion, authIn.DeviceName, authIn.ExpiresAt); err != nil {
		return nil, nil, err
	}

	const claimQ = `
update plugin_login_attempts
set completed_session_id = $2
where id = $1 and completed_session_id is null`
	tag, err := tx.Exec(ctx, claimQ, attemptID, authIn.ID)
	if err != nil {
		return nil, nil, err
	}
	if tag.RowsAffected() == 0 {
		return &attempt, nil, ErrConflict
	}
	attempt.CompletedSessionID = strPtr(authIn.ID)

	const getSessionQ = `
select id, user_id, refresh_token_hash, client_platform, coalesce(client_version, ''), coalesce(device_name, ''), created_at, last_seen_at, expires_at, revoked_at
from auth_sessions
where id = $1`
	var sess model.AuthSession
	if err := tx.QueryRow(ctx, getSessionQ, authIn.ID).Scan(
		&sess.ID, &sess.UserID, &sess.RefreshTokenHash, &sess.ClientPlatform, &sess.ClientVersion, &sess.DeviceName,
		&sess.CreatedAt, &sess.LastSeenAt, &sess.ExpiresAt, &sess.RevokedAt,
	); err != nil {
		return nil, nil, err
	}

	if err := tx.Commit(ctx); err != nil {
		return nil, nil, err
	}
	return &attempt, &sess, nil
}

func (s *Store) RecordRelayHealth(ctx context.Context, in RelayHealthInput) error {
	// Join to relay_instances so we can validate that the reported instance_id matches
	// what is actually bound to the session. Mismatched or stale reporters are rejected.
	const q = `
insert into relay_health_events
  (session_id, relay_instance_id, observed_at, ingest_active, egress_active, session_uptime_seconds, payload_json, created_at)
select
  s.id, s.relay_instance_id, $2, $3, $4, $5, $6, now()
from sessions s
join relay_instances ri on ri.id = s.relay_instance_id
where s.id = $1 and ri.instance_id = $7`
	tag, err := s.db.Exec(ctx, q, in.SessionID, in.ObservedAt, in.IngestActive, in.EgressActive, in.SessionUptimeSeconds, in.RawPayload, in.InstanceID)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return fmt.Errorf("%w: session not found or instance_id mismatch", ErrRelayHealthRejected)
	}

	_, err = s.db.Exec(ctx, `update relay_instances ri set last_health_at = $2 where ri.id = (select relay_instance_id from sessions where id = $1)`, in.SessionID, in.ObservedAt)
	return err
}

func (s *Store) FinalActivateSession(ctx context.Context, sessionID string) error {
	const q = `update sessions set status = 'active', provision_step = 'ready', updated_at = now() where id = $1 and status = 'provisioning'`
	tag, err := s.db.Exec(ctx, q, sessionID)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return fmt.Errorf("final_activate: no provisioning session for id=%s", sessionID)
	}
	return nil
}

func (s *Store) ListRelayManifest(ctx context.Context) ([]model.RelayManifestEntry, error) {
	const q = `
select region, ami_id, default_instance_type, updated_at
from relay_manifests
order by region asc`

	rows, err := s.db.Query(ctx, q)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := make([]model.RelayManifestEntry, 0)
	for rows.Next() {
		var e model.RelayManifestEntry
		if err := rows.Scan(&e.Region, &e.AMIID, &e.DefaultInstanceType, &e.UpdatedAt); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return out, nil
}

func (s *Store) UpsertRelayManifest(ctx context.Context, entries []model.RelayManifestEntry) error {
	if len(entries) == 0 {
		return nil
	}

	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)

	const q = `
insert into relay_manifests (region, ami_id, default_instance_type, updated_at)
values ($1, $2, $3, now())
on conflict (region)
do update set
  ami_id = excluded.ami_id,
  default_instance_type = excluded.default_instance_type,
  updated_at = now()`
	for _, e := range entries {
		if _, err := tx.Exec(ctx, q, e.Region, e.AMIID, e.DefaultInstanceType); err != nil {
			return err
		}
	}
	return tx.Commit(ctx)
}

func (s *Store) CleanupExpiredIdempotencyRecords(ctx context.Context) error {
	_, err := s.db.Exec(ctx, `delete from idempotency_records where expires_at <= now()`)
	return err
}

func (s *Store) RollupLiveSessionDurations(ctx context.Context) error {
	const q = `
update sessions
set duration_seconds = greatest(
      duration_seconds,
      floor(extract(epoch from (now() - started_at)))::integer
    ),
    updated_at = now()
where status in ('active', 'grace')
  and started_at <= now()`
	_, err := s.db.Exec(ctx, q)
	return err
}

func (s *Store) ReconcileOutageFromHealth(ctx context.Context) error {
	const q = `
with latest as (
  select distinct on (session_id)
    session_id,
    session_uptime_seconds
  from relay_health_events
  order by session_id, observed_at desc, id desc
)
update sessions s
set reconciled_seconds = greatest(s.reconciled_seconds, latest.session_uptime_seconds),
    duration_seconds = greatest(s.duration_seconds, latest.session_uptime_seconds),
    updated_at = now()
from latest
where s.id = latest.session_id
  and s.status in ('active', 'grace', 'stopped')`
	_, err := s.db.Exec(ctx, q)
	return err
}

func (s *Store) UpsertUsageRollups(ctx context.Context) error {
	const q = `
insert into usage_records
  (id, user_id, session_id, cycle_start_at, cycle_end_at, measured_seconds, reconciled_seconds, billable_seconds, overage_seconds, created_at, updated_at)
select
  'use_' || s.id,
  s.user_id,
  s.id,
  u.cycle_start_at,
  u.cycle_end_at,
  s.duration_seconds,
  s.reconciled_seconds,
  greatest(s.duration_seconds, s.reconciled_seconds),
  0,
  now(),
  now()
from sessions s
join users u on u.id = s.user_id
where s.status in ('active', 'grace', 'stopped')
  and s.started_at >= u.cycle_start_at
  and s.started_at <= u.cycle_end_at
on conflict (id)
do update set
  measured_seconds = excluded.measured_seconds,
  reconciled_seconds = excluded.reconciled_seconds,
  billable_seconds = excluded.billable_seconds,
  updated_at = now()`
	_, err := s.db.Exec(ctx, q)
	return err
}

func (s *Store) GetUserRelaySlug(ctx context.Context, userID string) (string, error) {
	var slug string
	err := s.db.QueryRow(ctx, `SELECT relay_slug FROM users WHERE id = $1`, userID).Scan(&slug)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return "", ErrNotFound
		}
		return "", err
	}
	return slug, nil
}

func (s *Store) UpdateProvisionStep(ctx context.Context, sessionID, step string) error {
	const q = `update sessions set provision_step = $2, updated_at = now() where id = $1 and status = 'provisioning'`
	_, err := s.db.Exec(ctx, q, sessionID, step)
	return err
}

// GetUserEIP returns the Elastic IP allocation ID and public IP for a user.
// Returns empty strings (no error) if the user has no EIP allocated.
func (s *Store) GetUserEIP(ctx context.Context, userID string) (allocationID string, publicIP string, err error) {
	err = s.db.QueryRow(ctx,
		`SELECT coalesce(eip_allocation_id, ''), coalesce(host(eip_public_ip), '') FROM users WHERE id = $1`,
		userID).Scan(&allocationID, &publicIP)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return "", "", ErrNotFound
		}
		return "", "", err
	}
	return allocationID, publicIP, nil
}

// SetUserEIP stores the Elastic IP allocation ID and public IP for a user.
func (s *Store) SetUserEIP(ctx context.Context, userID, allocationID, publicIP string) error {
	const q = `UPDATE users SET eip_allocation_id = $2, eip_public_ip = $3::inet WHERE id = $1`
	_, err := s.db.Exec(ctx, q, userID, allocationID, publicIP)
	return err
}

func strPtr(v string) *string {
	if v == "" {
		return nil
	}
	return &v
}

func nullableString(v string) any {
	if v == "" {
		return nil
	}
	return v
}

func generateStreamToken() (string, error) {
	b := make([]byte, 4)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

// AssignRelay atomically picks the least-loaded healthy server in region (falling
// back to any region), increments its current_sessions, and inserts a relay_assignments
// row. Returns ErrNoRelayCapacity if no server is available.
func (s *Store) AssignRelay(ctx context.Context, userID, sessionID, connectionID, region, streamToken string) (*model.RelayAssignment, error) {
	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)

	// Pick least-loaded healthy server — prefer region, fall back to any.
	const pickQ = `
SELECT rp.server_id, rp.host, host(rp.ip) as ip
FROM relay_pool rp
WHERE rp.status = 'active'
  AND rp.health_status = 'healthy'
  AND rp.current_sessions < rp.max_sessions
ORDER BY
    CASE WHEN rp.region = $1 THEN 0 ELSE 1 END,
    rp.current_sessions ASC
LIMIT 1
FOR UPDATE SKIP LOCKED`

	var serverID, host, ip string
	err = tx.QueryRow(ctx, pickQ, region).Scan(&serverID, &host, &ip)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNoRelayCapacity
		}
		return nil, err
	}

	// Increment session count.
	const incrQ = `UPDATE relay_pool SET current_sessions = current_sessions + 1 WHERE server_id = $1`
	if _, err := tx.Exec(ctx, incrQ, serverID); err != nil {
		return nil, err
	}

	// Insert assignment row.
	const insertQ = `
INSERT INTO relay_assignments (user_id, session_id, connection_id, server_id, stream_token)
VALUES ($1, $2, NULLIF($3, ''), $4, $5)
RETURNING id`
	var id int
	if err := tx.QueryRow(ctx, insertQ, userID, sessionID, connectionID, serverID, streamToken).Scan(&id); err != nil {
		return nil, err
	}

	if err := tx.Commit(ctx); err != nil {
		return nil, err
	}

	return &model.RelayAssignment{
		ID:           id,
		UserID:       userID,
		SessionID:    sessionID,
		ConnectionID: connectionID,
		ServerID:     serverID,
		StreamToken:  streamToken,
		Host:         host,
		IP:           ip,
	}, nil
}

// GetRelayAssignment returns the active assignment for a session (released_at IS NULL).
func (s *Store) GetRelayAssignment(ctx context.Context, sessionID string) (*model.RelayAssignment, error) {
	const q = `
SELECT ra.id, ra.user_id, ra.session_id, coalesce(ra.connection_id,''), ra.server_id, ra.stream_token,
       rp.host, host(rp.ip)
FROM relay_assignments ra
JOIN relay_pool rp ON rp.server_id = ra.server_id
WHERE ra.session_id = $1 AND ra.released_at IS NULL
LIMIT 1`

	var a model.RelayAssignment
	if err := s.db.QueryRow(ctx, q, sessionID).Scan(
		&a.ID, &a.UserID, &a.SessionID, &a.ConnectionID, &a.ServerID, &a.StreamToken, &a.Host, &a.IP,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	return &a, nil
}

// ReleaseRelay marks the assignment released and decrements current_sessions.
// Idempotent: safe to call if already released.
func (s *Store) ReleaseRelay(ctx context.Context, sessionID string) error {
	const q = `
UPDATE relay_pool SET current_sessions = GREATEST(0, current_sessions - 1)
WHERE server_id = (
    SELECT server_id FROM relay_assignments
    WHERE session_id = $1 AND released_at IS NULL
)
RETURNING server_id`
	var serverID string
	err := s.db.QueryRow(ctx, q, sessionID).Scan(&serverID)
	if err != nil && !errors.Is(err, pgx.ErrNoRows) {
		return err
	}

	const markQ = `UPDATE relay_assignments SET released_at = NOW() WHERE session_id = $1 AND released_at IS NULL`
	_, err = s.db.Exec(ctx, markQ, sessionID)
	return err
}

// ListActiveRelayServers returns all active relay pool servers for health monitoring.
func (s *Store) ListActiveRelayServers(ctx context.Context) ([]model.RelayPoolServer, error) {
	const q = `
SELECT server_id, provider, host, host(ip), region, status, current_sessions, max_sessions, coalesce(health_status,'unknown')
FROM relay_pool
WHERE status IN ('active', 'draining')
ORDER BY server_id`

	rows, err := s.db.Query(ctx, q)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []model.RelayPoolServer
	for rows.Next() {
		var srv model.RelayPoolServer
		if err := rows.Scan(&srv.ServerID, &srv.Provider, &srv.Host, &srv.IP, &srv.Region, &srv.Status, &srv.CurrentSessions, &srv.MaxSessions, &srv.HealthStatus); err != nil {
			return nil, err
		}
		out = append(out, srv)
	}
	return out, rows.Err()
}

// UpdateRelayServerHealth sets health_status and last_health_check for a pool server.
func (s *Store) UpdateRelayServerHealth(ctx context.Context, serverID, healthStatus string) error {
	const q = `UPDATE relay_pool SET health_status = $2, last_health_check = NOW() WHERE server_id = $1`
	_, err := s.db.Exec(ctx, q, serverID, healthStatus)
	return err
}
