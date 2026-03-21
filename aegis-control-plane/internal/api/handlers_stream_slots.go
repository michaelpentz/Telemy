package api

import (
	"errors"
	"net/http"
	"strconv"

	"github.com/go-chi/chi/v5"
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

type updateStreamSlotLabelRequest struct {
	Label string `json:"label"`
}

func (s *Server) handleUpdateStreamSlotLabel(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}

	slotNumber, err := strconv.Atoi(chi.URLParam(r, "slotNumber"))
	if err != nil {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "slotNumber must be an integer")
		return
	}

	var req updateStreamSlotLabelRequest
	if code, msg := decodeJSON(r, &req); code != 0 {
		writeAPIError(w, code, "invalid_request", msg)
		return
	}

	if err := s.store.UpdateStreamSlotLabel(r.Context(), userID, slotNumber, req.Label); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusNotFound, "not_found", "stream slot not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to update stream slot label")
		return
	}

	w.WriteHeader(http.StatusNoContent)
}
