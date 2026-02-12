mod app;
mod config;
mod security;
mod metrics;
mod aggregate;
mod server;
mod exporters;
mod model;
mod startup;
mod tray;

#[tokio::main]
async fn main() {
    init_logging();
    if let Err(err) = app::run().await {
        tracing::error!(error = %err, "fatal");
        std::process::exit(1);
    }
}

fn init_logging() {
    use tracing_subscriber::{fmt, EnvFilter};
    let filter = EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info"));
    fmt().with_env_filter(filter).with_target(false).init();
}
