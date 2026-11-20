pub mod boot;
pub mod embind;
pub mod fetch;
pub mod futex;
pub mod gl;
pub mod http;
pub mod mailbox;
pub mod opfs;
pub mod rna;
pub mod stubs;
pub mod thread;

/// Every host side module, in one place: the main store, each worker and the
/// completions thread all need the same set.
pub fn register_all(l: &mut wasmtime::Linker<crate::state::St>) -> anyhow::Result<()> {
    embind::register(l)?;
    boot::register(l)?;
    fetch::register(l)?;
    http::register(l)?;
    crate::fibre::register(l)?;
    futex::register(l)?;
    mailbox::register(l)?;
    gl::register(l)?;
    opfs::register(l)?;
    rna::register(l)?;
    stubs::register(l)?;
    thread::register(l)?;
    Ok(())
}
