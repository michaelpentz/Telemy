use std::os::windows::ffi::OsStrExt;
use windows::core::PCWSTR;
use windows::Win32::System::Registry::{
    RegDeleteValueW, RegOpenKeyExW, RegSetValueExW, HKEY_CURRENT_USER, KEY_SET_VALUE, REG_SZ,
};

pub fn set_autostart(app_name: &str, enable: bool) -> Result<(), Box<dyn std::error::Error>> {
    let exe = std::env::current_exe()?;
    let exe = exe.to_string_lossy().to_string();

    let key_path = to_wide("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    let mut hkey = HKEY_CURRENT_USER;

    unsafe {
        RegOpenKeyExW(
            HKEY_CURRENT_USER,
            PCWSTR(key_path.as_ptr()),
            0,
            KEY_SET_VALUE,
            &mut hkey,
        )?;

        if enable {
            let value = to_wide(&exe);
            let bytes = std::slice::from_raw_parts(value.as_ptr() as *const u8, value.len() * 2);
            RegSetValueExW(
                hkey,
                PCWSTR(to_wide(app_name).as_ptr()),
                0,
                REG_SZ,
                Some(bytes),
            )?;
        } else {
            let _ = RegDeleteValueW(hkey, PCWSTR(to_wide(app_name).as_ptr()));
        }
    }

    Ok(())
}

fn to_wide(s: &str) -> Vec<u16> {
    std::ffi::OsStr::new(s)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::to_wide;

    #[test]
    fn to_wide_is_null_terminated() {
        let v = to_wide("Telemy");
        assert!(!v.is_empty());
        assert_eq!(*v.last().unwrap(), 0);
    }

    #[test]
    fn to_wide_includes_content() {
        let v = to_wide("A");
        assert_eq!(v[0], 'A' as u16);
        assert_eq!(v[1], 0);
    }
}
