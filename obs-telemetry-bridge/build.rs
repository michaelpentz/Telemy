fn main() {
    #[cfg(windows)]
    {
        let mut res = winres::WindowsResource::new();
        res.set_icon("assets\\telemy.ico");
        res.set_icon_with_id("assets\\telemy.ico", "tray_default");
        let _ = res.compile();
    }
}
