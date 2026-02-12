use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use tray_item::{IconSource, TrayItem};
use tokio::sync::watch;

pub fn start_tray(
    dashboard_url: String,
    shutdown_flag: Arc<AtomicBool>,
    shutdown_tx: watch::Sender<bool>,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut tray = TrayItem::new("Telemy", IconSource::Resource("tray_default"))?;

    let open_url = dashboard_url.clone();
    tray.add_menu_item("Open Dashboard", move || {
        let _ = Command::new("cmd").args(["/C", "start", "", &open_url]).spawn();
    })?;

    let quit_flag = shutdown_flag.clone();
    let quit_tx = shutdown_tx.clone();
    tray.add_menu_item("Quit", move || {
        quit_flag.store(true, Ordering::SeqCst);
        let _ = quit_tx.send(true);
    })?;

    loop {
        if shutdown_flag.load(Ordering::SeqCst) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(250));
    }

    Ok(())
}
