package model

type UserStreamSlot struct {
	SlotNumber  int    `json:"slot_number"`
	Label       string `json:"label"`
	StreamToken string `json:"stream_token"`
}
