package store

import (
	"context"
	"errors"

	"github.com/jackc/pgx/v5"

	"github.com/telemyapp/aegis-control-plane/internal/model"
)

func (s *Store) ListUserStreamSlots(ctx context.Context, userID string) ([]model.UserStreamSlot, error) {
	const q = `
select slot_number, coalesce(label, ''), stream_token
from user_stream_slots
where user_id = $1
order by slot_number asc, stream_token asc`

	rows, err := s.db.Query(ctx, q, userID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := make([]model.UserStreamSlot, 0)
	for rows.Next() {
		var slot model.UserStreamSlot
		if err := rows.Scan(&slot.SlotNumber, &slot.Label, &slot.StreamToken); err != nil {
			return nil, err
		}
		out = append(out, slot)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		return nil, ErrNotFound
	}
	return out, nil
}

func (s *Store) UpdateStreamSlotLabel(ctx context.Context, userID string, slotNumber int, label string) error {
	const q = `
update user_stream_slots
set label = $3
where user_id = $1 and slot_number = $2`

	tag, err := s.db.Exec(ctx, q, userID, slotNumber, label)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) GetUserStreamSlotByToken(ctx context.Context, userID, streamToken string) (*model.UserStreamSlot, error) {
	const q = `
select slot_number, coalesce(label, ''), stream_token
from user_stream_slots
where user_id = $1 and stream_token = $2
limit 1`

	var slot model.UserStreamSlot
	if err := s.db.QueryRow(ctx, q, userID, streamToken).Scan(&slot.SlotNumber, &slot.Label, &slot.StreamToken); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	return &slot, nil
}
