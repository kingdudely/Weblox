//! The FMOD ScriptProcessor bridge, with a real output device.
//!
//! The glue's audio keys do four things: make a context, make a
//! ScriptProcessor, connect it, and copy the mixed guest floats into the
//! processor's output buffers.  Here the "processor" is a cpal stream and the
//! copy lands in a ring the callback drains.
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};

use crate::state::St;

pub const RATE: u32 = 48000;

struct Out {
    node: Option<(usize, usize)>, // buffer frames, channels
    ring: VecDeque<f32>,
    running: bool,
    generation: u64,
}

static OUT: Mutex<Option<Out>> = Mutex::new(None);
static ON: AtomicBool = AtomicBool::new(false);

thread_local! {
    static STREAM: std::cell::RefCell<Option<cpal::Stream>> = const { std::cell::RefCell::new(None) };
}

fn with<R>(f: impl FnOnce(&mut Out) -> R) -> R {
    let mut g = OUT.lock().unwrap();
    f(g.get_or_insert_with(|| Out { node: None, ring: VecDeque::new(), running: false, generation: 1 }))
}

pub fn sample_rate() -> f64 {
    ON.store(true, Ordering::Relaxed);
    RATE as f64
}

pub fn enabled() -> bool {
    std::env::var_os("WC_AUDIO").is_some()
}

pub fn init() -> f64 {
    if !enabled() { return RATE as f64 }
    ON.store(true, Ordering::Relaxed);
    with(|o| o.node = o.node.take());
    RATE as f64
}

pub fn create_processor(frames: usize) {
    ON.store(true, Ordering::Relaxed);
    with(|o| {
        let ch = o.node.map(|n| n.1).unwrap_or(2).max(1);
        o.node = Some((frames.max(256), ch));
    });
}

pub fn connect() {
    if !enabled() { return }
    ON.store(true, Ordering::Relaxed);
    let (frames, ch) = with(|o| {
        o.running = true;
        o.node.unwrap_or((4096, 2))
    });
    with(|o| {
        o.ring.clear();
        o.ring.reserve(frames * ch * 8);
    });
    let gen = with(|o| {
        o.generation = o.generation.wrapping_add(1);
        o.generation
    });
    STREAM.with(|s| {
        let mut slot = s.borrow_mut();
        *slot = None;
        let host = cpal::default_host();
        let Some(dev) = host.default_output_device() else {
            eprintln!("[aud ] no output device");
            return;
        };
        let mut cfg = match dev.default_output_config() {
            Ok(c) => c.config(),
            Err(e) => { eprintln!("[aud ] config: {e}"); return }
        };
        cfg.channels = ch as u16;
        cfg.buffer_size = cpal::BufferSize::Fixed(frames as u32);
        let cur_gen = gen;
        let stream = dev.build_output_stream(
            cfg,
            move |data: &mut [f32], _| {
                if !ON.load(Ordering::Relaxed) {
                    data.fill(0.0);
                    return;
                }
                let mut g = OUT.lock().unwrap();
                let Some(o) = g.as_mut() else { data.fill(0.0); return };
                if o.generation != cur_gen { data.fill(0.0); return }
                for v in data.iter_mut() {
                    *v = o.ring.pop_front().unwrap_or(0.0);
                }
            },
            |e| eprintln!("[aud ] stream: {e}"),
            None,
        );
        match stream {
            Ok(st) => {
                if let Err(e) = st.play() { eprintln!("[aud ] play: {e}") }
                *slot = Some(st);
                eprintln!("[aud ] stream up {ch}ch {frames}");
            }
            Err(e) => eprintln!("[aud ] build: {e}"),
        }
    });
}

pub fn suspend() {
    with(|o| o.running = false);
    STREAM.with(|s| { if let Some(st) = s.borrow().as_ref() { let _ = st.pause(); } });
}

pub fn resume() {
    with(|o| o.running = true);
    STREAM.with(|s| { if let Some(st) = s.borrow().as_ref() { let _ = st.play(); } });
}

pub fn stop() {
    STREAM.with(|s| *s.borrow_mut() = None);
    with(|o| { o.running = false; o.ring.clear() });
}

/// Push the mixed guest floats (planar, `channels` planes of `frames`) into the
/// ring, interleaved.
pub fn copy(st: &wasmtime::Store<St>, p: u32, offset: u32, frames: u32, channels: u32) {
    if p == 0 || frames == 0 || channels == 0 { return }
    let d = st.data().mem.data();
    let n = frames as usize;
    let ch = channels as usize;
    with(|o| {
        if o.node.is_none() { o.node = Some((4096, ch)) }
        for i in 0..n {
            for c in 0..ch {
                let at = p as usize + 4 * (c * n + i);
                if at + 4 > d.len() { return }
                let v = unsafe { *(d[at].get() as *const f32) };
                o.ring.push_back(v);
            }
        }
        let _ = offset;
    });
}

/// How many frames to mix now, if the device is pulling.
pub fn want() -> Option<u32> {
    if !enabled() || !ON.load(Ordering::Relaxed) { return None }
    with(|o| {
        let (frames, ch) = o.node?;
        if !o.running { return None }
        if STREAM.with(|s| s.borrow().is_none()) { return None }
        // keep ~two buffers queued
        let free = frames * ch * 3 - o.ring.len();
        if free >= frames * ch { Some(frames as u32) } else { None }
    })
}
