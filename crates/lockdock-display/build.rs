fn native_build() -> cc::Build {
    let mut build = cc::Build::new();
    build
        .flag("-std=c11")
        .flag("-Wno-unused-function")
        .include("native")
        .file("native/display.c");
    build
}

fn main() {
    println!("cargo:rerun-if-changed=native/display.c");
    println!("cargo:rerun-if-changed=native/display.h");

    native_build().compile("lockdock_display_native");

    // Keep the test variant in its own archive. It is only linked by unit tests,
    // where the process resolver and all assertions are implemented in Rust.
    let mut tests = native_build();
    tests.define("LOCKDOCK_DISPLAY_TEST", None);
    tests.define("proc_pidpath", Some("lockdock_display_test_proc_pidpath"));
    for symbol in [
        "lockdock_display_get_active_displays",
        "lockdock_display_copy_bounds",
        "lockdock_display_is_accessibility_trusted",
        "lockdock_display_copy_dock_window_bounds",
        "lockdock_display_copy_accessibility_dock_window_bounds",
        "lockdock_display_dock_overlay_active",
    ] {
        tests.define(symbol, Some(format!("{symbol}_test").as_str()));
    }
    tests.compile("lockdock_display_test_native");

    println!("cargo:rustc-link-lib=framework=CoreGraphics");
    println!("cargo:rustc-link-lib=framework=ApplicationServices");
    println!("cargo:rustc-link-lib=framework=ColorSync");
    println!("cargo:rustc-link-lib=framework=CoreFoundation");
}
