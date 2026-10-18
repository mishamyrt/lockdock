fn main() {
    println!("cargo:rerun-if-changed=native/display.c");
    println!("cargo:rerun-if-changed=native/display.h");

    cc::Build::new()
        .flag("-std=c11")
        .flag("-Wno-unused-function")
        .include("native")
        .file("native/display.c")
        .compile("lockdock_display_native");

    println!("cargo:rustc-link-lib=framework=CoreGraphics");
    println!("cargo:rustc-link-lib=framework=ApplicationServices");
    println!("cargo:rustc-link-lib=framework=ColorSync");
    println!("cargo:rustc-link-lib=framework=CoreFoundation");
}
