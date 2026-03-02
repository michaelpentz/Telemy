use std::sync::{Mutex, MutexGuard};

/// Extension trait for `std::sync::Mutex` that recovers from poison errors
/// instead of panicking. A poisoned mutex indicates a prior thread panicked
/// while holding the lock — the data may be inconsistent, but for our use
/// cases (config reads, status snapshots, vault access) recovering the inner
/// data is preferable to cascading panics through the application.
///
/// Note: this crate intentionally uses `std::sync::Mutex` (not `tokio::sync::Mutex`)
/// even in async contexts. All lock acquisitions are scoped blocks that never
/// hold the guard across an `.await` point, so the synchronous mutex is correct
/// and avoids the overhead of async-aware locking.
pub trait MutexExt<T> {
    fn lock_or_recover(&self) -> MutexGuard<'_, T>;
}

impl<T> MutexExt<T> for Mutex<T> {
    fn lock_or_recover(&self) -> MutexGuard<'_, T> {
        self.lock().unwrap_or_else(|poisoned| {
            tracing::warn!("recovered poisoned mutex");
            poisoned.into_inner()
        })
    }
}
