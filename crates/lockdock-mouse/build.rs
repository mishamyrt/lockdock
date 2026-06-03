fn main() {
    println!("cargo:rerun-if-changed=native/mouse.c");
    println!("cargo:rerun-if-changed=native/mouse.h");

    cc::Build::new()
        .flag("-std=c11")
        .flag("-Wno-unused-function")
        .include("native")
        .file("native/mouse.c")
        .compile("lockdock_mouse_native");

    println!("cargo:rustc-link-lib=framework=CoreGraphics");
    println!("cargo:rustc-link-lib=framework=ApplicationServices");
    println!("cargo:rustc-link-lib=framework=CoreFoundation");
}
