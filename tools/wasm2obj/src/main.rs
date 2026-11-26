//! Compile a core WebAssembly module directly to a relocatable ELF object.
//!
//! This deliberately does NOT construct a `wasmtime::Engine` or any runtime
//! state. It uses only the compiler half of wasmtime-environ plus the vendored
//! `wasmtime-internal-cranelift` compiler and writes the `object::write::Object`
//! that the compiler produces.

use anyhow::{Context, Result, bail};
use rayon::prelude::*;
use std::{
    any::Any,
    collections::{HashMap, HashSet},
    env,
    sync::atomic::{AtomicUsize, Ordering},
    time::Instant,
};
use wasmparser::{Parser, Validator, WasmFeatures};
use wasmtime_environ::{
    Collector, FuncKey, FunctionBodyData, Inlining,
    ModuleEnvironment, ModuleTypesBuilder, ObjectKind, StaticModuleIndex, Tunables,
};

fn main() -> Result<()> {
    let mut wasm_path = None;
    let mut out_path = "out.o".to_string();
    let mut max_funcs: Option<usize> = None;
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--out" => out_path = args.next().context("--out needs a path")?,
            "--all" => max_funcs = Some(usize::MAX),
            "--max-funcs" => {
                max_funcs = Some(args.next().context("--max-funcs needs a number")?.parse()?);
            }
            "--help" => {
                println!("usage: wasm2obj [--max-funcs N | --all] [--out FILE] INPUT.wasm");
                return Ok(());
            }
            other if other.starts_with('-') => bail!("unknown option {other}"),
            other => wasm_path = Some(other.to_string()),
        }
    }
    let wasm_path = wasm_path.context("missing input wasm path")?;
    // A bounded default: the full 154k-function module takes far longer than a
    // smoke test needs. Use --all to attempt every defined function.
    let max_funcs = max_funcs.unwrap_or(16);

    let t_start = Instant::now();
    let wasm = std::fs::read(&wasm_path)
        .with_context(|| format!("failed to read {}", wasm_path))?;
    eprintln!(
        "[t={:7.3}s] read {} ({} bytes)",
        t_start.elapsed().as_secs_f64(),
        wasm_path,
        wasm.len()
    );

    // ---------- compiler ----------
    let mut builder = wasmtime_internal_cranelift::builder(None)?;
    let mut tunables = Tunables::default_host();
    // Emscripten uses GC/reference types for exception handling (exnref).
    tunables.collector = Some(Collector::DeferredReferenceCounting);
    tunables.inlining = Inlining::No; // one serious iteration: skip call-graph inlining
    tunables.debug_native = false;
    tunables.debug_guest = false;
    tunables.generate_address_map = false;
    builder.set_tunables(tunables.clone())?;
    // Faster codegen for a large module; correctness of emitted object is the
    // goal here, not optimized native code.
    builder.set("opt_level", "none")?;
    let compiler = builder.build()?;
    eprintln!(
        "[t={:7.3}s] compiler built for {}",
        t_start.elapsed().as_secs_f64(),
        compiler.triple()
    );

    // ---------- front end: parse + validate + translate ----------
    let mut features = WasmFeatures::WASM3;
    features |= WasmFeatures::SHARED_EVERYTHING_THREADS;
    features |= WasmFeatures::LEGACY_EXCEPTIONS;
    let mut validator = Validator::new_with_features(features);
    let mut types = ModuleTypesBuilder::new(&validator);
    let mut parser = Parser::new(0);
    parser.set_features(features);

    let t_translate = Instant::now();
    let mut translation = ModuleEnvironment::new(
        &tunables,
        &mut validator,
        &mut types,
        StaticModuleIndex::from_u32(0),
    )
    .translate(parser, &wasm)
    .map_err(anyhow::Error::from)
    .context("failed to parse/translate WebAssembly module")?;
    translation.finalize_memory_init(&tunables, compiler.page_size_align(), u64::MAX, &mut types);
    translation.finalize_table_init(&tunables, &mut types);
    eprintln!(
        "[t={:7.3}s] translation done in {:.3}s: {} defined functions, {} imports, {} memories",
        t_start.elapsed().as_secs_f64(),
        t_translate.elapsed().as_secs_f64(),
        translation.function_body_inputs.len(),
        translation.module.imports().len(),
        translation.module.memories.len(),
    );

    let functions = std::mem::take(&mut translation.function_body_inputs);
    let num_funcs = functions.len();
    let limit = max_funcs.min(num_funcs);

    // ---------- compile function bodies ----------
    let module = StaticModuleIndex::from_u32(0);
    let t_compile = Instant::now();
    let inlining = compiler.inlining_compiler();
    let mut indices: HashMap<FuncKey, usize> = HashMap::new();
    let progress = AtomicUsize::new(0);
    let items: Vec<_> = functions.into_iter().take(limit).collect();
    let mut funcs: Vec<(String, FuncKey, Box<dyn Any + Send + Sync>)> = items
        .into_par_iter()
        .map(|(def_idx, body)| -> Result<_> {
            let key = FuncKey::DefinedWasmFunction(module, def_idx);
            let symbol = format!("_wasm_function_{}", def_idx.as_u32());
            let wasm_body = body.body.clone();
            let mut compiled = compiler
                .compile_function(&translation, key, body, &types, &symbol)
                .with_context(|| format!("compile_function({symbol})"))?;
            if let Some(inl) = inlining {
                inl.finish_compiling(&mut compiled, Some(wasm_body), &symbol)?;
            }
            let done = progress.fetch_add(1, Ordering::Relaxed) + 1;
            if done % 10000 == 0 {
                eprintln!(
                    "[t={:7.3}s] compiled {}/{} functions",
                    t_start.elapsed().as_secs_f64(),
                    done,
                    limit
                );
            }
            Ok((symbol, key, compiled.code))
        })
        .collect::<Result<Vec<_>>>()?;
    for (idx, (_, key, _)) in funcs.iter().enumerate() {
        indices.insert(*key, idx);
    }
    eprintln!(
        "[t={:7.3}s] compiled {limit}/{num_funcs} functions in {:.3}s",
        t_start.elapsed().as_secs_f64(),
        t_compile.elapsed().as_secs_f64(),
    );

    // ---------- builtins referenced by relocation targets ----------
    let mut builtins = HashSet::new();
    for (_, _, code) in &funcs {
        for target in compiler.compiled_function_relocation_targets(&**code) {
            match target {
                FuncKey::WasmToBuiltinTrampoline(b) | FuncKey::PatchableToBuiltinTrampoline(b) => {
                    builtins.insert((target, b));
                }
                _ => {}
            }
        }
    }
    for (key, builtin) in builtins {
        let symbol = match key {
            FuncKey::WasmToBuiltinTrampoline(_) => format!("wasmtime_builtin_{}", builtin.name()),
            _ => format!("wasmtime_patchable_builtin_{}", builtin.name()),
        };
        let mut compiled = compiler
            .compile_trampoline(None, key, &types, &symbol)
            .with_context(|| format!("compile_trampoline({symbol})"))?;
        if let Some(inl) = inlining {
            inl.finish_compiling(&mut compiled, None, &symbol)?;
        }
        let idx = funcs.len();
        funcs.push((symbol, key, compiled.code));
        indices.insert(key, idx);
    }

    // ---------- unresolved intra-module callees (only when bounded) ----------
    // When only a prefix of the module was compiled, calls to later functions
    // have no index. Emit an array-to-wasm trampoline as a stand-in so that the
    // object is still linkable; this is purely an artifact-generation aid.
    if limit < num_funcs {
        let mut missing = HashSet::new();
        for (_, _, code) in &funcs {
            for target in compiler.compiled_function_relocation_targets(&**code) {
                if !indices.contains_key(&target) {
                    missing.insert(target);
                }
            }
        }
        for key in missing {
            if let FuncKey::DefinedWasmFunction(_, def_idx) = key {
                let symbol = format!("_stub_function_{}", def_idx.as_u32());
                match compiler.compile_trampoline(
                    Some(&translation),
                    FuncKey::ArrayToWasmTrampoline(module, def_idx),
                    &types,
                    &symbol,
                ) {
                    Ok(mut compiled) => {
                        if let Some(inl) = inlining {
                            inl.finish_compiling(&mut compiled, None, &symbol)?;
                        }
                        let idx = funcs.len();
                        funcs.push((symbol, key, compiled.code));
                        indices.insert(key, idx);
                    }
                    Err(e) => {
                        eprintln!("warning: cannot stub {key:?}: {e}");
                        indices.insert(key, 0);
                    }
                }
            } else {
                eprintln!("warning: unresolved relocation target {key:?}; mapping to 0");
                indices.insert(key, 0);
            }
        }
    }

    // ---------- object emission ----------
    let t_obj = Instant::now();
    let mut obj = compiler.object(ObjectKind::Module)?;
    // wasmtime marks its artifacts with a private OSABI + e_flags. A plain .o
    // for a normal linker should use the generic ELF values.
    obj.flags = object::FileFlags::Elf {
        os_abi: object::elf::OsAbi(0),
        e_flags: object::elf::FileFlags(0),
        abi_version: 0,
    };
    let symbols = compiler.append_code(&mut obj, &funcs, &|_, callee| {
        indices
            .get(&callee)
            .copied()
            .unwrap_or_else(|| panic!("cannot resolve relocation to {callee:?}"))
    })?;
    // wasmtime-internal-cranelift defines functions with `Compilation` (local)
    // scope. Promote them to linker-visible globals so a C++ main() can refer
    // to `_wasm_function_N` with `extern "C"` declarations.
    for (sym, _) in symbols {
        if let Some(sym) = sym {
            obj.symbol_mut(sym).scope = object::SymbolScope::Linkage;
        }
    }
    let bytes = obj.write()?;
    std::fs::write(&out_path, &bytes)
        .with_context(|| format!("failed to write {out_path}"))?;
    eprintln!(
        "[t={:7.3}s] object emitted in {:.3}s: {} bytes -> {} ({} functions)",
        t_start.elapsed().as_secs_f64(),
        t_obj.elapsed().as_secs_f64(),
        bytes.len(),
        out_path,
        funcs.len(),
    );
    Ok(())
}

// Keep the otherwise-unused import honest: `FunctionBodyData` shows up in the
// public signature of `compile_function` and is referenced in docs/comments.
#[allow(dead_code)]
fn _assert_body_type<'a>(_: FunctionBodyData<'a>) {}
