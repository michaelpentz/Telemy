package config

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

type Config struct {
	ListenAddr                 string
	DatabaseURL                string
	JWTSecret                  string
	RelaySharedKey             string
	DefaultRegion              string
	SupportedRegion            []string
	RelayProvider              string
	AWSAMIMap                  map[string]string
	AWSInstanceType            string
	AWSSubnetID                string
	AWSSecurityIDs             []string
	AWSKeyName                 string
	RelayDomain                string
	AuthAccessTTL              time.Duration
	AuthRefreshTTL             time.Duration
	AuthPublicBaseURL          string
	PluginLoginAttemptTTL      time.Duration
	PluginLoginPollIntervalSec int
	PluginLoginCompleteKey     string
}

func LoadFromEnv() (Config, error) {
	cfg := Config{
		ListenAddr:                 envOrDefault("AEGIS_LISTEN_ADDR", ":8080"),
		DatabaseURL:                os.Getenv("AEGIS_DATABASE_URL"),
		JWTSecret:                  os.Getenv("AEGIS_JWT_SECRET"),
		RelaySharedKey:             os.Getenv("AEGIS_RELAY_SHARED_KEY"),
		DefaultRegion:              envOrDefault("AEGIS_DEFAULT_REGION", "us-east-1"),
		SupportedRegion:            splitCSV(envOrDefault("AEGIS_SUPPORTED_REGIONS", "us-east-1,eu-west-1")),
		RelayProvider:              envOrDefault("AEGIS_RELAY_PROVIDER", "fake"),
		AWSAMIMap:                  parseKVMap(os.Getenv("AEGIS_AWS_AMI_MAP")),
		AWSInstanceType:            envOrDefault("AEGIS_AWS_INSTANCE_TYPE", "t4g.small"),
		AWSSubnetID:                os.Getenv("AEGIS_AWS_SUBNET_ID"),
		AWSSecurityIDs:             splitCSV(os.Getenv("AEGIS_AWS_SECURITY_GROUP_IDS")),
		AWSKeyName:                 os.Getenv("AEGIS_AWS_KEY_NAME"),
		RelayDomain:                envOrDefault("CLOUDFLARE_RELAY_DOMAIN", "relay.telemyapp.com"),
		AuthAccessTTL:              time.Duration(ParsePositiveIntEnv("AEGIS_AUTH_ACCESS_TOKEN_MINUTES", 15)) * time.Minute,
		AuthRefreshTTL:             time.Duration(ParsePositiveIntEnv("AEGIS_AUTH_REFRESH_TOKEN_DAYS", 30)) * 24 * time.Hour,
		AuthPublicBaseURL:          envOrDefault("AEGIS_AUTH_PUBLIC_BASE_URL", "https://telemyapp.com"),
		PluginLoginAttemptTTL:      time.Duration(ParsePositiveIntEnv("AEGIS_PLUGIN_LOGIN_ATTEMPT_MINUTES", 10)) * time.Minute,
		PluginLoginPollIntervalSec: ParsePositiveIntEnv("AEGIS_PLUGIN_LOGIN_POLL_INTERVAL_SECONDS", 3),
		PluginLoginCompleteKey:     os.Getenv("AEGIS_PLUGIN_LOGIN_COMPLETE_KEY"),
	}

	if cfg.DatabaseURL == "" {
		return Config{}, fmt.Errorf("AEGIS_DATABASE_URL is required")
	}
	if cfg.JWTSecret == "" {
		return Config{}, fmt.Errorf("AEGIS_JWT_SECRET is required")
	}
	if cfg.RelaySharedKey == "" {
		return Config{}, fmt.Errorf("AEGIS_RELAY_SHARED_KEY is required")
	}
	if cfg.PluginLoginCompleteKey == "" {
		return Config{}, fmt.Errorf("AEGIS_PLUGIN_LOGIN_COMPLETE_KEY is required")
	}
	if cfg.RelayProvider != "fake" && cfg.RelayProvider != "aws" && cfg.RelayProvider != "byor" {
		return Config{}, fmt.Errorf("AEGIS_RELAY_PROVIDER must be one of fake|aws|byor")
	}
	if cfg.RelayProvider == "aws" && len(cfg.AWSAMIMap) == 0 {
		return Config{}, fmt.Errorf("AEGIS_AWS_AMI_MAP is required for aws relay provider")
	}
	return cfg, nil
}

func envOrDefault(k, v string) string {
	if raw := os.Getenv(k); raw != "" {
		return raw
	}
	return v
}

func splitCSV(v string) []string {
	parts := strings.Split(v, ",")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		s := strings.TrimSpace(p)
		if s != "" {
			out = append(out, s)
		}
	}
	return out
}

func ParsePositiveIntEnv(k string, d int) int {
	raw := os.Getenv(k)
	if raw == "" {
		return d
	}
	n, err := strconv.Atoi(raw)
	if err != nil || n <= 0 {
		return d
	}
	return n
}

func parseKVMap(v string) map[string]string {
	out := make(map[string]string)
	if strings.TrimSpace(v) == "" {
		return out
	}
	pairs := strings.Split(v, ",")
	for _, p := range pairs {
		parts := strings.SplitN(strings.TrimSpace(p), "=", 2)
		if len(parts) != 2 {
			continue
		}
		k := strings.TrimSpace(parts[0])
		val := strings.TrimSpace(parts[1])
		if k != "" && val != "" {
			out[k] = val
		}
	}
	return out
}
