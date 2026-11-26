#!/usr/bin/env python3
"""Regenerate the Wasmtime-ABI import table for the native C++ host.

Sources of truth
----------------
* ``/tmp/opencode/WebClient.raknet.wasm`` -- type section (params/results) and
  the import section (order, module/name, type indices).
* ``docs/imports-wasm.json`` -- the checked-in copy of the 342-entry import
  list (1 memory + 341 functions).  It is cross-checked against the binary;
  a mismatch is a hard error.

Outputs
-------
* ``host/imports.h`` -- exactly 341 ``extern "C"`` declarations with the hidden
  vmctx pointer in the first (rdi) slot, plus the slot metadata/install API.
* ``host/imports.cpp`` -- one log-once-and-return-zero stub per unimplemented
  slot, plus a preserved hand-written region for the real implementations.

The hand-written region between the markers is copied verbatim on regeneration,
so adding a name to ``REALIZED`` (and writing its implementation) promotes an
import from stub to real without ever being overwritten.

Usage::

    python3 host/gen_imports.py \
        --wasm /tmp/opencode/WebClient.raknet.wasm \
        --docs /home/john/Weblox/docs/imports-wasm.json \
        --out-dir host
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Import names with a hand-written implementation in imports.cpp.  Every name
# must exist in the module or the generator fails (catches typos early).
REALIZED = {
    # console
    "emscripten_outn", "emscripten_errn", "emscripten_console_log",
    "emscripten_console_warn", "emscripten_console_error",
    "_emscripten_log_formatted",
    # embind registration + binding table
    "_embind_register_function", "_embind_register_void", "_embind_register_bool",
    "_embind_register_integer", "_embind_register_bigint", "_embind_register_float",
    "_embind_register_std_string", "_embind_register_std_wstring",
    "_embind_register_emval", "_embind_register_memory_view",
    # fetch / HTTP (libcurl)
    "js_fetch_file", "emscripten_fetch_free",
    "_emscripten_fetch_get_response_headers_length",
    "_emscripten_fetch_get_response_headers",
    # pthreads, mailboxes, futex
    "__asyncjs__rbxFutexWaitMainJspi", "_emscripten_thread_set_strongref",
    "_emscripten_thread_mailbox_await", "__pthread_create_js",
    "_emscripten_thread_cleanup", "_emscripten_notify_mailbox_postmessage",
    "_emscripten_init_main_thread_js", "_emscripten_receive_on_main_thread_js",
    "emscripten_unwind_to_js_event_loop", "emscripten_check_blocking_allowed",
    "emscripten_runtime_keepalive_push", "_emscripten_runtime_keepalive_clear",
    # time / entropy / heap / host info / WASI-lite / small boot helpers
    "emscripten_get_now", "emscripten_date_now", "get_hardware_concurrency",
    "emscripten_num_logical_cores", "random_get", "emscripten_get_heap_max",
    "emscripten_resize_heap", "HaveOffsetConverter",
    "RBX_setFullWasmStackTrace", "RBX_restoreWasmStackTraceLimit",
    "get_effective_device_pixel_ratio", "emscripten_get_device_pixel_ratio",
    "is_mobile_browser", "is_touch_mobile", "is_touch_screen_device",
    "has_keyboard", "has_mouse", "js_web_client_page_visible",
    "emscripten_get_canvas_element_size", "emscripten_set_canvas_element_size",
    "read_host_info_int", "read_host_info_string", "proc_exit",
    "clock_time_get", "environ_sizes_get", "environ_get", "__syscall_getegid32",
    "__syscall_geteuid32", "__syscall_getgid32", "__syscall_getuid32",
    "_tzset_js", "_mktime_js", "_localtime_js", "_gmtime_js",
    "__assert_fail", "_abort_js", "exit", "js_linking_open_url",
    # EM_ASM (key/signature/argument-buffer decoding, no instance needed)
    "emscripten_asm_const_int", "emscripten_asm_const_double",
    "emscripten_asm_const_int_sync_on_main_thread",
    "emscripten_asm_const_async_on_main_thread",
}

VAL_TYPES = {0x7F: "i32", 0x7E: "i64", 0x7D: "f32", 0x7C: "f64",
             0x7B: "v128", 0x70: "funcref", 0x6F: "externref"}
C_TYPES = {"i32": "int32_t", "i64": "int64_t", "f32": "float", "f64": "double"}
ZEROS = {"i32": "0", "i64": "0", "f32": "0.0f", "f64": "0.0", "void": "0"}

BEGIN_HAND = "// === BEGIN HANDWRITTEN IMPLEMENTATIONS === (preserved by gen_imports.py)"
END_HAND = "// === END HANDWRITTEN IMPLEMENTATIONS ==="


# ---------------------------------------------------------------------------
# Minimal wasm binary reader (type + import sections only; the 100 MB module is
# memory-mapped by the OS and we stop reading after the import section).
# ---------------------------------------------------------------------------
def uleb(data: bytes, pos: int) -> tuple[int, int]:
    val = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        val |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return val, pos
        shift += 7


def wname(data: bytes, pos: int) -> tuple[str, int]:
    n, pos = uleb(data, pos)
    return data[pos:pos + n].decode("utf-8"), pos + n


def parse_wasm(path: Path) -> tuple[list[tuple[list[int], list[int]]], list[tuple]]:
    data = path.read_bytes()
    if data[:4] != b"\0asm":
        raise ValueError(f"{path} is not a wasm module")
    pos = 8
    types: list[tuple[list[int], list[int]]] = []
    imports: list[tuple] = []
    while pos < len(data):
        sec = data[pos]
        pos += 1
        size, pos = uleb(data, pos)
        end = pos + size
        if sec == 1:                                   # type section
            count, pos = uleb(data, pos)
            for _ in range(count):
                if data[pos] != 0x60:
                    raise ValueError("unsupported type form")
                pos += 1
                np, pos = uleb(data, pos)
                params = list(data[pos:pos + np])
                pos += np
                nr, pos = uleb(data, pos)
                results = list(data[pos:pos + nr])
                pos += nr
                types.append((params, results))
        elif sec == 2:                                 # import section
            count, pos = uleb(data, pos)
            for _ in range(count):
                module, pos = wname(data, pos)
                name, pos = wname(data, pos)
                kind = data[pos]
                pos += 1
                ti = None
                if kind == 0:                          # func
                    ti, pos = uleb(data, pos)
                elif kind == 1:                        # table
                    pos += 1                           # elem type
                    flags = data[pos]
                    pos += 1
                    _, pos = uleb(data, pos)           # min
                    if flags & 1:
                        _, pos = uleb(data, pos)       # max
                elif kind == 2:                        # memory
                    flags = data[pos]
                    pos += 1
                    _, pos = uleb(data, pos)           # min
                    if flags & 1:
                        _, pos = uleb(data, pos)       # max
                elif kind == 3:                        # global
                    pos += 2                           # type + mutability
                elif kind == 4:                        # tag
                    pos += 1
                    _, pos = uleb(data, pos)
                else:
                    raise ValueError(f"unknown import kind {kind}")
                imports.append((module, name, kind, ti))
        pos = end
        if sec == 2:
            break
    return types, imports


def sym(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def sig_str(params: list[str], result: str) -> str:
    return "(" + ", ".join(params) + ") -> " + result


# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------
def build_header(rows: list[dict]) -> str:
    out = [
        "// Generated by host/gen_imports.py -- DO NOT EDIT BY HAND.\n",
        "// 341 Wasmtime-ABI imports, in wasm import order (slot 1..341).\n",
        "#pragma once\n\n",
        "#include <cstddef>\n#include <cstdint>\n\n",
        "namespace wb {\n\n",
        "// Slot N is wasm import index N.  The first defined wasm function index\n",
        "// is 341 (there are 341 imported functions before it).\n",
        "constexpr std::size_t kImportCount = ", str(len(rows)), ";\n",
        "constexpr std::uint32_t kFirstDefinedFunction = ", str(len(rows)), ";\n\n",
        "struct ImportDesc {\n",
        "  std::uint32_t slot;      // 1-based index into the VMContext import array\n",
        "  const char* module;      // always \"env\" for this module\n",
        "  const char* name;\n",
        "  const char* sig;         // human-readable wasm signature\n",
        "  std::uint32_t type_index;  // type section index\n",
        "};\n\n",
        "// Static table, indexed by slot - 1, valid for the life of the process.\n",
        "const ImportDesc* import_descs();\n",
        "inline const ImportDesc* import_desc(std::uint32_t slot) {\n",
        "  return slot >= 1 && slot <= kImportCount ? &import_descs()[slot - 1] : nullptr;\n",
        "}\n\n",
        "// Raw wasm-call function pointers, indexed by slot - 1.  Each has the\n",
        "// hidden vmctx in rdi followed by the wasm arguments in SysV registers.\n",
        "void* const* import_wasm_calls();\n",
        "void* import_wasm_call(std::uint32_t slot);\n\n",
        "// Mirror of wasmtime-environ's VMFunctionImport (32 bytes on x86-64).\n",
        "// Used by imports_install() so this header does not depend on vmctx.h.\n",
        "struct FunctionImportSlot {\n",
        "  void* array_call;    // array-call ABI; no trampoline is emitted here\n",
        "  void* wasm_call;     // <- filled from import_wasm_call()\n",
        "  std::uint32_t type_index;\n",
        "  std::uint32_t reserved;\n",
        "  void* vmctx;         // the importing instance's VMContext\n",
        "};\n",
        "static_assert(sizeof(FunctionImportSlot) == 32, \"check ABI mirror\");\n\n",
        "// Fill slots[0..count) from the generated table.  `import_vmctx` is stored\n",
        "// in every slot's vmctx field and becomes the hidden rdi argument.\n",
        "// array_call is left null: direct wasm calls use wasm_call; see IMPORTS.md.\n",
        "void imports_install(FunctionImportSlot* slots, std::size_t count,\n",
        "                     void* import_vmctx);\n\n",
        "// Real implementations need linear memory.  Call once from host setup with\n",
        "// the shared memory base and its current byte length.\n",
        "void imports_set_memory(void* base, std::size_t length);\n",
        "bool imports_has_memory();\n\n",
        "// Called by every generated stub the first time that slot is invoked.\n",
        "void import_log_first(std::uint32_t slot);\n\n",
        "// --- import table accessors used by the hand-written implementations ---\n",
        "// Fixed-size, process-lifetime storage so pointers stay valid.\n",
        "struct Binding {\n",
        "  char name[96];\n  char sig[48];\n",
        "  std::uint32_t argc;\n  std::uint32_t invoker;\n  std::uint32_t id;\n",
        "};\n",
        "const Binding* embind_bindings();\nstd::size_t embind_binding_count();\n",
        "const Binding* embind_find(const char* name);\n",
        "const char* embind_type(std::uint32_t id);\n\n",
        "// Host hooks for the parts of a port that must re-enter wasm.  See\n",
        "// IMPORTS.md: these need the table base / a worker VMContext, which is\n",
        "// owned by vmctx.cpp, and are deliberately not faked here.\n",
        "using ThreadEntry = void (*)(std::uint32_t start, std::uint32_t arg,\n",
        "                             std::uint32_t pthread_ptr);\n",
        "using WasmCallback = double (*)(void* vmctx, std::uint32_t table_index,\n",
        "                                const double* args, std::size_t argc);\n",
        "void imports_set_thread_entry(ThreadEntry entry);\n",
        "void imports_set_wasm_callback(WasmCallback cb);\n",
        "void imports_set_main_thread(std::uint32_t pthread_ptr);\n\n",
        "// Wake waiters of a futex word.  Wire the memory.atomic.notify builtin to\n",
        "// this so the ported __asyncjs__rbxFutexWaitMainJspi can be notified.\n",
        "std::uint32_t imports_futex_notify(std::uint32_t addr, std::uint32_t count);\n\n",
        "// Feed the emscripten_fetch response-header table when the async fetch\n",
        "// path (slot 304, still a stub) is implemented by the host.\n",
        "void imports_store_headers(std::uint32_t fetch, const char* text);\n\n",
        "}  // namespace wb\n\n",
        'extern "C" {\n',
    ]
    for row in rows:
        args = ", ".join(["void* vmctx"] + [f"{C_TYPES[t]} a{i}" for i, t in enumerate(row["params"])])
        out.append(f"// slot {row['slot']:3}: {row['module']}.{row['name']}  {row['sig']}  (type {row['type_index']})\n")
        out.append(f"{row['ret_c']} wasm_import_{row['slot']:03d}_{sym(row['name'])}({args});\n")
    out.append("}  // extern \"C\"\n")
    return "".join(out)


def build_cpp(rows: list[dict], handwritten: str) -> str:
    out = [
        "// Generated by host/gen_imports.py -- the marked real-implementation\n",
        "// region below is preserved verbatim on regeneration.\n",
        '#include "imports.h"\n\n',
        "#include <algorithm>\n#include <atomic>\n#include <chrono>\n",
        "#include <condition_variable>\n#include <cstdio>\n#include <cstdlib>\n",
        "#include <cstring>\n#include <memory>\n#include <mutex>\n#include <string>\n",
        "#include <sys/stat.h>\n#include <thread>\n#include <unordered_map>\n",
        "#include <unordered_set>\n#include <vector>\n\n",
        "#include <curl/curl.h>\n\n",
        "namespace wb {\nnamespace {\n",
        "std::atomic<bool> g_seen[kImportCount + 1];\n",
        "const ImportDesc g_descs[] = {\n",
    ]
    for row in rows:
        out.append(
            f'  {{ {row["slot"]}, "{row["module"]}", "{row["name"]}", '
            f'"{row["sig"]}", {row["type_index"]} }},\n'
        )
    out.extend([
        "};\n",
        "void* const g_calls[kImportCount] = {\n",
    ])
    for row in rows:
        out.append(f'  reinterpret_cast<void*>(&wasm_import_{row["slot"]:03d}_{sym(row["name"])}),\n')
    out.extend([
        "};\n",
        "}  // namespace\n\n",
        "const ImportDesc* import_descs() { return g_descs; }\n",
        "void* const* import_wasm_calls() { return g_calls; }\n",
        "void* import_wasm_call(std::uint32_t slot) {\n",
        "  return slot >= 1 && slot <= kImportCount ? g_calls[slot - 1] : nullptr;\n",
        "}\n\n",
        "void import_log_first(std::uint32_t slot) {\n",
        "  if (slot && slot <= kImportCount &&\n",
        "      !g_seen[slot].exchange(true, std::memory_order_relaxed)) {\n",
        "    const ImportDesc& d = g_descs[slot - 1];\n",
        '    std::fprintf(stderr, "[import] first call slot=%u %s.%s\\n", slot, d.module, d.name);\n',
        "  }\n",
        "}\n",
        "}  // namespace wb\n\n",
        "// --- generated stubs: log once, return a safe zero ----------------------\n",
        "// === BEGIN GENERATED STUBS ===\n",
    ])
    for row in rows:
        if row["real"]:
            out.append(f"// slot {row['slot']:3}: {row['name']} -- hand-written below\n")
            continue
        args = ", ".join(["void* vmctx"] + [f"{C_TYPES[t]} a{i}" for i, t in enumerate(row["params"])])
        out.append(f"extern \"C\" {row['ret_c']} wasm_import_{row['slot']:03d}_{sym(row['name'])}({args}) {{\n")
        out.append("  (void)vmctx;\n")
        for i in range(len(row["params"])):
            out.append(f"  (void)a{i};\n")
        out.append(f"  wb::import_log_first({row['slot']});\n")
        if row["ret"] != "void":
            out.append(f"  return {ZEROS[row['ret']]};\n")
        out.append("}\n\n")
    out.extend([
        "// === END GENERATED STUBS ===\n\n",
        "// --- real implementations (preserved by the generator) -----------------\n",
        BEGIN_HAND, "\n",
        handwritten,
        "\n", END_HAND, "\n",
    ])
    return "".join(out)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--wasm", type=Path, default=Path("/tmp/opencode/WebClient.raknet.wasm"))
    ap.add_argument("--docs", type=Path, default=root / "docs" / "imports-wasm.json")
    ap.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parent)
    args = ap.parse_args()

    types, imports = parse_wasm(args.wasm)
    doc = None
    if args.docs.exists():
        doc = json.loads(args.docs.read_text())
        if len(doc) != len(imports):
            raise SystemExit(f"docs import count {len(doc)} != wasm {len(imports)}")
        for i, (d, (mod, name, kind, ti)) in enumerate(zip(doc, imports)):
            # The docs file mislabels the memory import (kind 2) as "table".
            want_kind = "func" if kind == 0 else "memory" if kind == 2 else str(kind)
            got_kind = d.get("kind")
            if (d["module"], d["name"], d.get("type")) != (mod, name, ti) or (
                got_kind not in (want_kind, "table" if kind == 2 else want_kind)
            ):
                raise SystemExit(f"import mismatch at {i}: docs={d} wasm={(mod, name, kind, ti)}")

    rows: list[dict] = []
    seen_names: set[str] = set()
    for slot, (mod, name, kind, ti) in enumerate(imports):
        if kind != 0:
            continue
        if name in seen_names:
            raise SystemExit(f"duplicate import name {name}")
        seen_names.add(name)
        params, results = types[ti]
        if len(results) > 1:
            raise SystemExit(f"multi-value import {name}: needs sret (not supported yet)")
        pnames = [VAL_TYPES.get(t) for t in params]
        if any(p is None for p in pnames):
            raise SystemExit(f"unsupported param type in {name}: {params}")
        ret = VAL_TYPES.get(results[0], "void") if results else "void"
        if ret != "void" and ret not in C_TYPES:
            raise SystemExit(f"unsupported result type in {name}: {ret}")
        rows.append({
            "slot": slot, "module": mod, "name": name, "type_index": ti,
            "params": pnames, "ret": ret, "ret_c": C_TYPES.get(ret, "void"),
            "sig": sig_str(pnames, ret), "real": name in REALIZED,
        })
    if len(rows) != 341:
        raise SystemExit(f"expected 341 function imports, got {len(rows)}")
    missing = REALIZED - seen_names
    if missing:
        raise SystemExit(f"REALIZED names not in module: {sorted(missing)}")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "imports.h").write_text(build_header(rows))
    cpp = args.out_dir / "imports.cpp"
    hand = ""
    if cpp.exists():
        old = cpp.read_text()
        if BEGIN_HAND in old and END_HAND in old:
            hand = old.split(BEGIN_HAND, 1)[1].split(END_HAND, 1)[0].strip("\n")
    cpp.write_text(build_cpp(rows, hand))
    print(f"generated {len(rows)} signatures "
          f"({sum(r['real'] for r in rows)} real, "
          f"{sum(not r['real'] for r in rows)} stubs) -> {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
