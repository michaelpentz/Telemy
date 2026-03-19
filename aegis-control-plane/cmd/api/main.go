package main

import (
	"context"
	"log"
	"net/http"
	"os/signal"
	"syscall"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/telemyapp/aegis-control-plane/internal/api"
	"github.com/telemyapp/aegis-control-plane/internal/config"
	"github.com/telemyapp/aegis-control-plane/internal/dns"
	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/relay"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

func main() {
	cfg, err := config.LoadFromEnv()
	if err != nil {
		log.Fatalf("load config: %v", err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	appCtx, appCancel := context.WithCancel(ctx)
	defer appCancel()

	pool, err := pgxpool.New(ctx, cfg.DatabaseURL)
	if err != nil {
		log.Fatalf("connect db: %v", err)
	}
	defer pool.Close()

	if err := pool.Ping(ctx); err != nil {
		log.Fatalf("ping db: %v", err)
	}

	st := store.New(pool, cfg.RelayDomain)
	manifestEntries := buildManifestEntries(cfg)
	if err := st.UpsertRelayManifest(ctx, manifestEntries); err != nil {
		log.Fatalf("sync relay manifest: %v", err)
	}
	var prov relay.Provisioner
	switch cfg.RelayProvider {
	case "aws":
		awsProv, err := relay.NewAWSProvisioner(relay.AWSProvisionerOptions{
			AMIByRegion:   cfg.AWSAMIMap,
			InstanceType:  cfg.AWSInstanceType,
			SubnetID:      cfg.AWSSubnetID,
			SecurityGroup: cfg.AWSSecurityIDs,
			KeyName:       cfg.AWSKeyName,
			EIPStore:      st,
		})
		if err != nil {
			log.Fatalf("init aws provisioner: %v", err)
		}
		prov = awsProv
	case "byor":
		prov = relay.NewStoreBackedBYORProvisioner(st)
	default:
		prov = relay.NewFakeProvisioner()
	}
	dnsClient := dns.NewClient(cfg.RelayDomain)
	appServer, handler := api.NewRouter(cfg, st, prov, dnsClient, api.WithAppContext(appCtx))

	srv := &http.Server{
		Addr:        cfg.ListenAddr,
		Handler:     handler,
		ReadTimeout: 30 * time.Second,
		// Relay provisioning in AWS mode may take >15s before the handler writes a response.
		WriteTimeout: 3 * time.Minute,
		IdleTimeout:  60 * time.Second,
	}

	go func() {
		<-ctx.Done()
		appCancel()
		// Wait for in-flight provisioning goroutines before tearing down HTTP.
		// The appCtx derived from ctx is already cancelled, so provisioning
		// contexts will hit their deadline; give them up to 30s to clean up.
		appServer.Shutdown(30 * time.Second)
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutdownCtx)
	}()

	log.Printf("WARNING: API server starting on plain HTTP (%s). Ensure a TLS-terminating proxy (e.g., Cloudflare) is in front.", cfg.ListenAddr)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("http server: %v", err)
	}
}

func buildManifestEntries(cfg config.Config) []model.RelayManifestEntry {
	manifestEntries := make([]model.RelayManifestEntry, 0, len(cfg.SupportedRegion))
	for _, region := range cfg.SupportedRegion {
		ami := cfg.AWSAMIMap[region]
		if ami == "" && (cfg.RelayProvider == "fake" || cfg.RelayProvider == "byor") {
			ami = "ami-fake-" + region
		}
		if ami == "" {
			continue
		}
		manifestEntries = append(manifestEntries, model.RelayManifestEntry{
			Region:              region,
			AMIID:               ami,
			DefaultInstanceType: cfg.AWSInstanceType,
		})
	}
	return manifestEntries
}
