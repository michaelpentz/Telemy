package api

import (
	"errors"
	"net/http"

	"github.com/telemyapp/aegis-control-plane/internal/auth"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

func (s *Server) handleUserStreamSlots(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}

	slots, err := s.store.ListUserStreamSlots(r.Context(), userID)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeJSON(w, http.StatusOK, map[string]any{"stream_slots": []map[string]any{}})
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to load stream slots")
		return
	}

	resp := make([]map[string]any, 0, len(slots))
	for _, slot := range slots {
		resp = append(resp, map[string]any{
			"slot_number":  slot.SlotNumber,
			"label":        slot.Label,
			"stream_token": slot.StreamToken,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"stream_slots": resp})
}
