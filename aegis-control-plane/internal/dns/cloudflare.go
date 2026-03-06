package dns

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
)

const defaultRelayDomain = "relay.telemyapp.com"

// Client manages Cloudflare DNS A records for relay subdomains.
type Client struct {
	zoneID     string
	apiToken   string
	baseDomain string
	httpClient *http.Client
}

// NewClient reads Cloudflare credentials from environment variables.
func NewClient() *Client {
	domain := os.Getenv("CLOUDFLARE_RELAY_DOMAIN")
	if domain == "" {
		domain = defaultRelayDomain
	}
	return &Client{
		zoneID:     os.Getenv("CLOUDFLARE_ZONE_ID"),
		apiToken:   os.Getenv("CLOUDFLARE_DNS_TOKEN"),
		baseDomain: domain,
		httpClient: &http.Client{},
	}
}

// Enabled returns true if both zoneID and apiToken are configured.
func (c *Client) Enabled() bool {
	return c.zoneID != "" && c.apiToken != ""
}

// FQDN returns the fully-qualified domain name for a relay slug.
func (c *Client) FQDN(slug string) string {
	return slug + "." + c.baseDomain
}

// CreateOrUpdateRecord upserts a Cloudflare A record for the given slug pointing to ip.
func (c *Client) CreateOrUpdateRecord(slug, ip string) error {
	if !c.Enabled() {
		log.Printf("dns: cloudflare not configured, skipping create for slug=%s", slug)
		return nil
	}
	fqdn := c.FQDN(slug)
	existingID, err := c.findRecord(fqdn)
	if err != nil {
		return fmt.Errorf("dns: find record: %w", err)
	}

	body := map[string]any{
		"type":    "A",
		"name":    fqdn,
		"content": ip,
		"ttl":     60,
		"proxied": false,
	}
	payload, err := json.Marshal(body)
	if err != nil {
		return err
	}

	var method, url string
	if existingID != "" {
		method = http.MethodPut
		url = fmt.Sprintf("https://api.cloudflare.com/client/v4/zones/%s/dns_records/%s", c.zoneID, existingID)
	} else {
		method = http.MethodPost
		url = fmt.Sprintf("https://api.cloudflare.com/client/v4/zones/%s/dns_records", c.zoneID)
	}

	req, err := http.NewRequest(method, url, bytes.NewReader(payload))
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+c.apiToken)
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("dns: cloudflare request: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		respBody, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("dns: cloudflare %s returned %d: %s", method, resp.StatusCode, string(respBody))
	}
	log.Printf("dns: %s record %s -> %s (record_id=%s)", method, fqdn, ip, existingID)
	return nil
}

// DeleteRecord removes the Cloudflare A record for the given slug.
func (c *Client) DeleteRecord(slug string) error {
	if !c.Enabled() {
		log.Printf("dns: cloudflare not configured, skipping delete for slug=%s", slug)
		return nil
	}
	fqdn := c.FQDN(slug)
	recordID, err := c.findRecord(fqdn)
	if err != nil {
		return fmt.Errorf("dns: find record: %w", err)
	}
	if recordID == "" {
		log.Printf("dns: no existing record for %s, nothing to delete", fqdn)
		return nil
	}

	url := fmt.Sprintf("https://api.cloudflare.com/client/v4/zones/%s/dns_records/%s", c.zoneID, recordID)
	req, err := http.NewRequest(http.MethodDelete, url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+c.apiToken)

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("dns: cloudflare request: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		respBody, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("dns: cloudflare DELETE returned %d: %s", resp.StatusCode, string(respBody))
	}
	log.Printf("dns: deleted record %s (record_id=%s)", fqdn, recordID)
	return nil
}

// findRecord queries Cloudflare for an existing A record matching fqdn.
func (c *Client) findRecord(fqdn string) (string, error) {
	url := fmt.Sprintf("https://api.cloudflare.com/client/v4/zones/%s/dns_records?type=A&name=%s", c.zoneID, fqdn)
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("Authorization", "Bearer "+c.apiToken)

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return "", fmt.Errorf("dns: cloudflare request: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		respBody, _ := io.ReadAll(resp.Body)
		return "", fmt.Errorf("dns: cloudflare GET returned %d: %s", resp.StatusCode, string(respBody))
	}

	var result struct {
		Result []struct {
			ID string `json:"id"`
		} `json:"result"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return "", fmt.Errorf("dns: decode response: %w", err)
	}
	if len(result.Result) == 0 {
		return "", nil
	}
	return result.Result[0].ID, nil
}
