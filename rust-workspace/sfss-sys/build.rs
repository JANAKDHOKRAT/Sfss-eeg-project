use std::path::PathBuf;
// PathBuf is used to build the output path for the generated bindings.rs file

fn main() {
    // Build script entry point for Cargo
    // This runs before compilation and prepares:
    //   - Rust linker settings
    //   - generated FFI bindings from the C header

    // 🔥 FORCE correct paths (no default fallback)
    let lib_dir = std::env::var("SFSS_LIB_DIR")
        .expect("SFSS_LIB_DIR not set");
    // Reads the environment variable that points to the directory containing libsfss_ffi.a
    // This value is used by Cargo/Rust during linking

    let include_dir = std::env::var("SFSS_INCLUDE")
        .expect("SFSS_INCLUDE not set");
    // Reads the environment variable that points to the directory containing sfss_ffi.h
    // This path is passed to bindgen and clang so the header can be parsed correctly

    println!("cargo:rustc-link-search=native={}", lib_dir);
    // Tells Cargo to add this directory to the native library search path
    // Rust linker will look here for static and dynamic libraries

    println!("cargo:rustc-link-lib=static=sfss_ffi");
    // Links the static library libsfss_ffi.a
    // This library is produced by the CMake build of the C++ SFSS code

    println!("cargo:rustc-link-lib=dylib=emp-tool");
    // Links the EMP toolkit shared library
    // Used indirectly by the SFSS C++ implementation

    println!("cargo:rustc-link-lib=dylib=gmp");
    // Links GMP, the GNU Multiple Precision library
    // Used by the C++ SFSS code for big integer arithmetic

    println!("cargo:rustc-link-lib=dylib=gmpxx");
    // Links the C++ GMP wrapper library
    // Required by the C++ SFSS build

    println!("cargo:rustc-link-lib=dylib=stdc++");
    // Links the C++ standard library
    // Needed because libsfss_ffi is compiled from C++

    println!("cargo:rerun-if-env-changed=SFSS_LIB_DIR");
    // Tells Cargo to rerun this build script if SFSS_LIB_DIR changes

    println!("cargo:rerun-if-env-changed=SFSS_INCLUDE");
    // Tells Cargo to rerun this build script if SFSS_INCLUDE changes

    let header_path = format!("{}/sfss_ffi.h", include_dir);
    // Builds the full path to the SFSS FFI header file
    // This is the header that bindgen will parse

    println!("cargo:warning=Using header: {}", header_path);
    // Prints a build-time warning so you can verify which header was used

    let bindings = bindgen::Builder::default()
        .header(header_path)
        // Tells bindgen which header to parse

        .clang_arg(format!("-I{}", include_dir))
        // Adds the include directory so clang can resolve includes like sfss.h, util.h, etc.

        .allowlist_function("sfss_.*")
        // Only generate Rust bindings for C functions whose names start with sfss_
        // Example: sfss_init, sfss_sdpf_gen, sfss_sdpf_enc, sfss_sdpf_eval, sfss_reconstruct

        .allowlist_var("SFSS_.*")
        // Only generate Rust bindings for constants/macros whose names start with SFSS_
        // Example: SFSS_KEY_SIZE, SFSS_STREAM_KEY_SIZE, SFSS_CTX_SIZE

        .generate()
        .expect("Failed to generate bindings");
    // Runs bindgen and produces Rust FFI code from the C header
    // The generated code will become bindings.rs

    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    // Gets Cargo's output directory for generated build artifacts

    bindings.write_to_file(out.join("bindings.rs")).unwrap();
    // Writes the generated Rust bindings into OUT_DIR/bindings.rs
    // This file is later included by:
    //   include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}