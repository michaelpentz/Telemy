use crate::aegis::ControlPlaneClient;
use crate::config::Config;
use crate::security::Vault;
use rand::{distributions::Alphanumeric, Rng};
use std::time::{SystemTime, UNIX_EPOCH};

/// Build a `ControlPlaneClient` from pre-loaded config and vault references.
pub fn build_aegis_client(
    config: &Config,
    vault: &Vault,
) -> Result<ControlPlaneClient, Box<dyn std::error::Error>> {
    let base_url = config
        .aegis
        .base_url
        .as_deref()
        .ok_or("missing aegis.base_url in config")?
        .trim();
    let jwt_key = config
        .aegis
        .access_jwt_key
        .as_deref()
        .ok_or("missing aegis.access_jwt_key in config")?
        .trim();
    if base_url.is_empty() {
        return Err("missing aegis.base_url in config".into());
    }
    if jwt_key.is_empty() {
        return Err("missing aegis.access_jwt_key in config".into());
    }
    let access_jwt = vault.retrieve(jwt_key)?;
    Ok(ControlPlaneClient::new(base_url, access_jwt.trim())?)
}

/// Generate a random alphanumeric token of the given length.
pub fn generate_token(len: usize) -> String {
    rand::thread_rng()
        .sample_iter(&Alphanumeric)
        .take(len)
        .map(char::from)
        .collect()
}

/// Generate a prefixed idempotency key with timestamp and random suffix.
pub fn generate_idempotency_key() -> String {
    let ts = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis();
    format!("telemy-{}-{}", ts, generate_token(12))
}

/// Build a `ControlPlaneClient` by loading config and vault from disk.
/// Used by the IPC module which doesn't hold references to app-level config/vault.
pub fn build_aegis_client_from_local_config() -> Result<ControlPlaneClient, String> {
    let config = Config::load().map_err(|e| e.to_string())?;
    let vault = Vault::new(config.vault.path.as_deref()).map_err(|e| e.to_string())?;
    build_aegis_client(&config, &vault).map_err(|e| e.to_string())
}
