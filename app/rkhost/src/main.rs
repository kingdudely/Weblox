// rkhost CLI — headless form of the same server the Tauri app embeds.

fn main() {
    let opts = rkhost::Options::from_args(std::env::args().skip(1));
    if std::env::args().any(|a| a == "--help" || a == "-h") {
        println!(
            "rkhost --wasm W --engine-dir D [--cookie FILE] [--port {}] [--bridge-port {}]",
            rkhost::DEFAULT_PORT,
            rkhost::DEFAULT_BRIDGE_PORT
        );
        return;
    }
    rkhost::serve(opts);
}
