package relay

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

// SLSError is a typed error returned by SLSClient methods.
type SLSError struct {
	Code    int
	Message string
}

func (e *SLSError) Error() string {
	return fmt.Sprintf("sls: HTTP %d: %s", e.Code, e.Message)
}

// Sentinel errors for common SLS API failures.
var (
	ErrSLSUnauthorized = &SLSError{Code: http.StatusUnauthorized, Message: "unauthorized"}
	ErrSLSNotFound     = &SLSError{Code: http.StatusNotFound, Message: "not found"}
	ErrSLSServerError  = &SLSError{Code: http.StatusInternalServerError, Message: "server error"}
)

// StreamID represents a single SLS stream entry.
type StreamID struct {
	Publisher   string `json:"publisher"`
	Player      string `json:"player"`
	Description string `json:"description"`
}

type slsListResponse struct {
	Data   []StreamID `json:"data"`
	Status string     `json:"status"`
}

// SLSClient talks to the SLS HTTP management API on a relay server.
type SLSClient struct {
	baseURL string
	apiKey  string
	http    *http.Client
}

// NewSLSClient creates a client for the SLS API at baseURL (e.g. http://kc1.relay.telemyapp.com:8090).
func NewSLSClient(baseURL, apiKey string) *SLSClient {
	return &SLSClient{
		baseURL: baseURL,
		apiKey:  apiKey,
		http:    &http.Client{Timeout: 10 * time.Second},
	}
}

// ListStreamIDs returns all stream IDs registered on the relay.
func (c *SLSClient) ListStreamIDs(ctx context.Context) ([]StreamID, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.baseURL+"/api/stream-ids", nil)
	if err != nil {
		return nil, fmt.Errorf("sls: build request: %w", err)
	}
	c.setAuth(req)

	resp, err := c.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("sls: list request: %w", err)
	}
	defer resp.Body.Close()

	if err := c.checkStatus(resp); err != nil {
		return nil, err
	}

	var body slsListResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		return nil, fmt.Errorf("sls: decode list response: %w", err)
	}
	return body.Data, nil
}

// CreateStreamID registers a new stream ID pair on the relay.
func (c *SLSClient) CreateStreamID(ctx context.Context, publisher, player, description string) error {
	payload := StreamID{
		Publisher:   publisher,
		Player:      player,
		Description: description,
	}
	b, err := json.Marshal(payload)
	if err != nil {
		return fmt.Errorf("sls: marshal create body: %w", err)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+"/api/stream-ids", bytes.NewReader(b))
	if err != nil {
		return fmt.Errorf("sls: build request: %w", err)
	}
	c.setAuth(req)
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.http.Do(req)
	if err != nil {
		return fmt.Errorf("sls: create request: %w", err)
	}
	defer resp.Body.Close()

	return c.checkStatus(resp)
}

// DeleteStreamID removes a stream ID by its publisher name.
func (c *SLSClient) DeleteStreamID(ctx context.Context, id string) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodDelete, c.baseURL+"/api/stream-ids/"+id, nil)
	if err != nil {
		return fmt.Errorf("sls: build request: %w", err)
	}
	c.setAuth(req)

	resp, err := c.http.Do(req)
	if err != nil {
		return fmt.Errorf("sls: delete request: %w", err)
	}
	defer resp.Body.Close()

	return c.checkStatus(resp)
}

func (c *SLSClient) setAuth(req *http.Request) {
	req.Header.Set("Authorization", "Bearer "+c.apiKey)
}

func (c *SLSClient) checkStatus(resp *http.Response) error {
	if resp.StatusCode >= 200 && resp.StatusCode < 300 {
		return nil
	}
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
	msg := string(body)
	if msg == "" {
		msg = resp.Status
	}
	switch resp.StatusCode {
	case http.StatusUnauthorized:
		return &SLSError{Code: resp.StatusCode, Message: "unauthorized"}
	case http.StatusNotFound:
		return &SLSError{Code: resp.StatusCode, Message: "not found"}
	default:
		return &SLSError{Code: resp.StatusCode, Message: msg}
	}
}
