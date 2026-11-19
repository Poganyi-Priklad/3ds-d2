//! A set of functions compiled to C (recompile.py's compile_set and its
//! helpers: may_block, flags_read_after, hookable, leaves, the parts).

use crate::analysis::Analysis;
use crate::cs::{OpKind, GRP_JUMP, REG_EIP};
use crate::emit::{x87_op, Emitter, Sig};
use crate::flags::Flags;
use crate::func::{Func, Tgt};
use crate::image::Image;
use crate::json::J;
use crate::omap::OMap;
use crate::program::{natives, Program, Summary, ERROR_PATH_FUNCS, NORETURN_FUNCS};
use crate::x86::*;
use crate::Refused;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet, VecDeque};
use std::time::Instant;

pub const BLOCKING_IMPORTS: [&str; 18] = [
    "Wait",
    "Sleep",
    "EnterCriticalSection",
    "ReadFile",
    "GetOverlappedResult",
    "GetMessage",
    "MsgWait",
    "SignalObjectAndWait",
    "DeviceIoControl",
    "recv",
    "send",
    "select",
    "connect",
    "accept",
    "gethostbyname",
    "DirectSound",
    "DirectDraw",
    "SwitchToThread",
];

/// A line on stderr every AOT_PROGRESS seconds (30; 0: off) in the long loops.
pub struct Progress {
    t0: Instant,
    last: Instant,
    every: f64,
}

impl Progress {
    pub fn new(t0: Instant) -> Progress {
        let every = std::env::var("AOT_PROGRESS").ok().and_then(|s| s.parse().ok()).unwrap_or(30.0);
        Progress { t0, last: Instant::now(), every }
    }
    pub fn tick(&mut self, what: &str, done: usize, total: usize) {
        let now = Instant::now();
        if self.every <= 0.0 || (now - self.last).as_secs_f64() < self.every {
            return;
        }
        self.last = now;
        let of = if total > 0 { format!(" of {} ({:.0}%)", total, 100.0 * done as f64 / total as f64) } else { String::new() };
        eprintln!("aot: {}: {}{}, {:.0} s", what, done, of, (now - self.t0).as_secs_f64());
    }
}

pub fn load_starts(tsv: &str) -> HashSet<u32> {
    let mut starts = HashSet::new();
    if let Ok(text) = std::fs::read_to_string(tsv) {
        for line in text.split_inclusive('\n').skip(1) {
            let a = line.split('\t').next().unwrap_or("");
            if let Ok(v) = parse_hex(a) {
                starts.insert(v);
            }
        }
    }
    starts
}

/// Python's int(s, 16): optional whitespace, sign, 0x, underscores between digits.
pub fn parse_hex(s: &str) -> Result<u32, ()> {
    let t = s.trim();
    let t = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")).unwrap_or(t);
    if t.is_empty() || t.starts_with('_') || t.ends_with('_') || t.contains("__") {
        return Err(());
    }
    let d: String = t.chars().filter(|&c| c != '_').collect();
    u64::from_str_radix(&d, 16).map(|v| v as u32).map_err(|_| ())
}

/// The whole instructions a trampoline copies (5 bytes or more) and the game's 16 bytes.
pub fn root_head(img: &Image, va: u32) -> Result<(u32, Vec<u8>), Refused> {
    let mut keep = 0u32;
    while keep < 5 {
        let i = img.insn(va + keep)?;
        if i.mnemonic == "call"
            || i.mnemonic == "jmp"
            || i.mnemonic == "ret"
            || i.mnemonic.starts_with('j')
            || i.ops.iter().any(|o| matches!(o.kind, OpKind::Mem(m) if m.base == REG_EIP))
        {
            return Err(Refused(format!("a branch in the first bytes at {:08x}: no trampoline", va)));
        }
        keep += i.size;
    }
    Ok((keep, (0..16).map(|k| img.u8(va + k)).collect()))
}

fn text_bytes(img: &Image) -> &[u8] {
    let lo = (img.text_lo - img.base) as usize;
    let hi = (lo + (img.text_hi - img.text_lo) as usize).min(img.mem.len());
    &img.mem[lo.min(hi)..hi]
}

fn i32_at(mem: &[u8], k: usize) -> i64 {
    let mut b = [0u8; 4];
    for j in 0..4 {
        b[j] = *mem.get(k + j).unwrap_or(&0);
    }
    i32::from_le_bytes(b) as i64
}

/// Of `want`, those a rel32 call, jmp or jcc anywhere in .text could reach.
pub fn rel32_targets(img: &Image, want: &HashSet<u32>) -> BTreeSet<u32> {
    let mut hit = BTreeSet::new();
    let mem = text_bytes(img);
    let n = mem.len() as i64 - 5;
    let mut k = 0i64;
    while k < n {
        let ku = k as usize;
        let b = mem[ku];
        let (nn, d) = if b == 0xE8 || b == 0xE9 {
            (k + 5, i32_at(mem, ku + 1))
        } else if b == 0x0F && (0x80..=0x8F).contains(&mem[ku + 1]) && ku + 6 <= mem.len() {
            (k + 6, i32_at(mem, ku + 2))
        } else {
            k += 1;
            continue;
        };
        let t = (img.text_lo as i64 + nn + d) as u32;
        if want.contains(&t) {
            hit.insert(t);
        }
        k += 1;
    }
    hit
}

/// The compiled functions whose C, with every compiled callee's, never calls Box86.
pub fn leaves(prog: &Program, compiled: &BTreeSet<u32>) -> HashSet<u32> {
    let direct = |va: u32| -> bool {
        let f = prog.func(va);
        for t in f.calls.values().chain(f.tails.values()) {
            if let Tgt::Direct(t) = t {
                if compiled.contains(t) || prog.intrinsics.contains_key(t) || prog.native_direct(*t) {
                    continue;
                }
            }
            return true;
        }
        false
    };
    let mut out: HashSet<u32> = compiled.iter().copied().filter(|&va| !direct(va)).collect();
    let mut changed = true;
    while changed {
        changed = false;
        let mut snap: Vec<u32> = out.iter().copied().collect();
        snap.sort();
        for va in snap {
            let f = prog.func(va);
            if f.calls.values().chain(f.tails.values()).any(|t| t.direct().is_some_and(|t| compiled.contains(&t) && !out.contains(&t))) {
                out.remove(&va);
                changed = true;
            }
        }
    }
    out
}

/// Of vas, those that may block their thread when Box86 runs them nested.
pub fn may_block(img: &Image, prog: &Program, vas: &HashSet<u32>) -> HashMap<u32, bool> {
    let mut out: HashMap<u32, bool> = HashMap::new();
    let skip = |f: &Func, site: &u32, t: &Tgt| -> bool {
        (f.noreturn.contains(site) && !f.exits.contains(site))
            || t.direct().is_some_and(|d| ERROR_PATH_FUNCS.contains(&d) || NORETURN_FUNCS.contains(&d))
    };
    let direct = |va: u32| -> bool {
        let f = prog.func(va);
        if f.refused.is_some() {
            return true;
        }
        for (site, t) in f.calls.iter().chain(f.tails.iter()) {
            if skip(&f, site, t) {
                continue;
            }
            match t {
                Tgt::Indirect => return true,
                Tgt::Import(slot) => {
                    let name = img.imports.get(slot).map(|x| x.1.as_str()).unwrap_or("");
                    if BLOCKING_IMPORTS.iter().any(|b| name.starts_with(b)) {
                        return true;
                    }
                }
                _ => {}
            }
        }
        false
    };
    let mut work: Vec<u32> = vas.iter().copied().collect();
    work.sort();
    for &va in &work {
        out.insert(va, direct(va));
    }
    let mut changed = true;
    while changed {
        changed = false;
        for &va in &work {
            if out[&va] {
                continue;
            }
            let f = prog.func(va);
            for (site, t) in f.calls.iter().chain(f.tails.iter()) {
                let Tgt::Direct(td) = t else { continue };
                if skip(&f, site, t) {
                    continue;
                }
                if prog.intrinsics.contains_key(td) || prog.native_direct(*td) {
                    continue;
                }
                if *out.get(td).unwrap_or(&true) {
                    out.insert(va, true);
                    changed = true;
                    break;
                }
            }
        }
    }
    out
}

fn reads_first(img: &Image, mut va: u32, depth: u32, steps: u32) -> Result<bool, Refused> {
    for _ in 0..steps {
        let i = img.insn(va)?;
        let (d, u) = flags_rw(&i);
        if u != 0 {
            return Ok(true);
        }
        if i.mnemonic == "call" {
            let op = i.ops[0];
            if depth == 0 && op.is_imm() && reads_first(img, op.imm() as u32, 1, steps)? {
                return Ok(true);
            }
            return Ok(false);
        }
        if d != 0 || i.mnemonic.starts_with("ret") || i.mnemonic.starts_with("jmp") || i.groups.contains(&GRP_JUMP) {
            return Ok(false);
        }
        va = va.wrapping_add(i.size);
    }
    Ok(false)
}

/// Of vas, the functions whose flags a caller may read after they return.
pub fn flags_read_after(img: &Image, vas: &BTreeSet<u32>) -> OMap<u32, u32> {
    let mut hit = OMap::new();
    let mem = text_bytes(img);
    let n = mem.len() as i64 - 5;
    let mut k = 0i64;
    while k < n {
        let ku = k as usize;
        if mem[ku] != 0xE8 {
            k += 1;
            continue;
        }
        let t = (img.text_lo as i64 + k + 5 + i32_at(mem, ku + 1)) as u32;
        if vas.contains(&t) && !hit.contains_key(&t) {
            if let Ok(true) = reads_first(img, img.text_lo + ku as u32 + 5, 0, 12) {
                hit.insert(t, img.text_lo + ku as u32);
            }
        }
        k += 1;
    }
    hit
}

/// A function's instructions as [lo, hi) runs of bytes, in order.
pub fn code_ranges(f: &Func) -> Vec<(u32, u32)> {
    let mut ranges: Vec<(u32, u32)> = Vec::new();
    for &a in &f.order {
        let e = a.wrapping_add(f.insns[&a].size);
        match ranges.last_mut() {
            Some(r) if r.1 == a => r.1 = e,
            _ => ranges.push((a, e)),
        }
    }
    ranges
}

/// FNV-1a over those bytes, as aot_spike.cpp's code_hash() takes it.
pub fn code_hash(img: &Image, ranges: &[(u32, u32)]) -> u32 {
    let mut h: u32 = 0x811c9dc5;
    for &(lo, hi) in ranges {
        let a = (lo - img.base) as usize;
        let b = ((hi - img.base) as usize).min(img.mem.len());
        for &x in &img.mem[a.min(b)..b] {
            h = (h ^ x as u32).wrapping_mul(0x01000193);
        }
    }
    h
}

/// Each compiled function's entry for the check: (keep, head) where it can be hooked, else why not.
pub fn hookable(img: &Image, prog: &Program) -> (BTreeMap<u32, (u32, Vec<u8>)>, BTreeMap<u32, String>) {
    let mut heads: BTreeMap<u32, (u32, Vec<u8>)> = BTreeMap::new();
    let mut why: BTreeMap<u32, String> = BTreeMap::new();
    for &va in &prog.compiled {
        match root_head(img, va) {
            Err(e) => {
                why.insert(va, e.0);
                continue;
            }
            Ok(h) => {
                if prog.summ[&va].pops.is_none() {
                    why.insert(va, "its ret pops aren't known".to_string());
                    continue;
                }
                heads.insert(va, h);
            }
        }
    }
    let mut inner: HashMap<u32, u32> = HashMap::new();
    for (&va, (keep, _)) in &heads {
        for k in 1..*keep {
            inner.insert(va + k, va);
        }
    }
    let mut region = inner.clone();
    for &va in heads.keys() {
        region.insert(va, va);
    }
    let mut keys = prog.func_keys();
    keys.sort();
    for va in keys {
        let f = prog.func(va);
        for (src, succ) in f.succ.iter() {
            let i = f.insns.get(src);
            for t in succ {
                if let Some(&it) = inner.get(t) {
                    let fall = i.is_some_and(|i| *t == src.wrapping_add(i.size)) && region.get(src) == Some(&it);
                    if !fall {
                        why.insert(it, format!("a branch from {:08x} into its first bytes ({:08x})", src, t));
                    }
                }
            }
        }
        for (src, t) in f.calls.iter().chain(f.tails.iter()) {
            if let Tgt::Direct(t) = t {
                if let Some(&it) = inner.get(t) {
                    why.insert(it, format!("a call or jump from {:08x} into its first bytes ({:08x})", src, t));
                }
            }
        }
    }
    let want: HashSet<u32> = inner.keys().copied().collect();
    for t in rel32_targets(img, &want) {
        why.entry(inner[&t]).or_insert_with(|| format!("a 32-bit branch somewhere in .text could reach its first bytes ({:08x})", t));
    }
    heads.retain(|va, _| !why.contains_key(va));
    (heads, why)
}

fn mask(rs: &[u8]) -> u32 {
    rs.iter().filter(|&&r| r != ESP).map(|&r| 1u32 << r).sum()
}

fn round3(v: f64) -> f64 {
    (v * 1000.0).round() / 1000.0
}

fn int_list(v: &[u8]) -> J {
    J::List(v.iter().map(|&x| J::Int(x as i64)).collect())
}

pub struct Compiled<'a> {
    pub prog: Program<'a>,
    pub report: J,
    pub refused: OMap<u32, String>,
    pub x86_insns: usize,
    pub x86_bytes: u64,
    pub leaves: usize,
    pub rounds: usize,
    pub analysis_s: f64,
    pub emit_s: f64,
}

fn calls_tails(f: &Func) -> impl Iterator<Item = (&u32, &Tgt)> {
    f.calls.iter().chain(f.tails.iter())
}

#[allow(clippy::too_many_arguments)]
pub fn compile_set<'a>(
    img: &'a Image,
    starts: &HashSet<u32>,
    roots: &[u32],
    extra: &[u32],
    mode_list: &[&'static str],
    outdir: &str,
    names: &HashMap<u32, String>,
    callouts: bool,
) -> Compiled<'a> {
    let t0 = Instant::now();
    let mut progress = Progress::new(t0);
    let mut all = starts.clone();
    all.extend(roots.iter().copied());
    all.extend(extra.iter().copied());
    let mut prog = Program::new(img, all, natives(), callouts);
    let mut want: Vec<u32> = roots.to_vec();
    for &v in extra {
        if !roots.contains(&v) {
            want.push(v);
        }
    }
    let mut reach = prog.closure(&want, 30000);
    reach.extend(want.iter().copied());
    let mut reach_sorted: Vec<u32> = reach.iter().copied().collect();
    reach_sorted.sort();
    eprintln!("aot: {} functions to compile, {} with their callees, {:.0} s", want.len(), reach.len(), t0.elapsed().as_secs_f64());
    let mut refused: OMap<u32, String> = OMap::new();
    // Thunks through a pointer whose pops nothing tells (issue #138): refused, as their decoding would be.
    prog.thunk_pops = Some(prog.compute_thunk_pops());
    for &va in &reach_sorted {
        let f = prog.func(va);
        let mut ptr: Vec<u32> = f.tails.iter().filter(|(_, t)| t.is_pointer()).map(|(k, _)| *k).collect();
        ptr.sort();
        if f.refused.is_none() && f.ret_pops.is_empty() && !ptr.is_empty() && !prog.thunk_pops.as_ref().unwrap().contains_key(&va) {
            let mut g = (*f).clone();
            g.refused = Some(format!("a thunk through a pointer at {:08x} whose pops aren't known", ptr[0]));
            prog.set_func(va, g);
        }
    }
    for &va in &want {
        let f = prog.func(va);
        if let Some(r) = &f.refused {
            refused.insert(va, r.clone());
            continue;
        }
        prog.compiled.insert(va);
    }
    let blocks = may_block(img, &prog, &reach);
    prog.apply_exits(&blocks);
    prog.summarize(&reach_sorted, 50);
    // What the analysis refuses whatever the signatures, screened once.
    let compiled_sorted: Vec<u32> = prog.compiled.iter().copied().collect();
    let screen = prog.callees_first(&compiled_sorted);
    let full_outs = set_of(&[EAX, ECX, EDX, EBX, EBP, ESI, EDI]);
    for (n, &va) in screen.iter().enumerate() {
        progress.tick("screened", n, screen.len());
        for attempt in 0..2 {
            let res: Result<(Regs, Option<Summary>), Refused> = (|| {
                let a = Analysis::new(&prog, prog.func(va), Some(full_outs), true)?;
                if !a.esp_ok {
                    return Err(Refused(format!("esp at a ret isn't known to be the entry's: {}", a.esp_why())));
                }
                Flags::new(&prog.func(va))?;
                let g = a.garbage;
                let s2 = if attempt == 1 {
                    let s2 = Analysis::new(&prog, prog.func(va), None, true)?;
                    Some(Summary {
                        reads: s2.live_in,
                        preserved: s2.preserved,
                        pops: if s2.esp_moved { None } else { prog.cfg_pops.get(&va).copied().flatten() },
                        known: true,
                    })
                } else {
                    None
                };
                Ok((g, s2))
            })();
            match res {
                Ok((g, s2)) => {
                    prog.garbage.insert(va, g);
                    if let Some(s2) = s2 {
                        prog.summ.insert(va, s2);
                    }
                    break;
                }
                Err(e) => {
                    if attempt == 0 && e.0.starts_with("esp at a ret") && prog.make_plain(va) {
                        continue;
                    }
                    refused.insert(va, e.0);
                    break;
                }
            }
        }
    }
    for (va, site) in flags_read_after(img, &prog.compiled).iter() {
        refused.insert(*va, format!("its flags are read after the call at {:08x}", site));
    }
    for va in refused.keys() {
        prog.compiled.remove(va);
    }
    prog.apply_exits(&blocks);
    prog.summarize(&reach_sorted, 50);
    let mut rounds = 0usize;
    let mut outs: HashMap<u32, Regs>;
    let sigs: HashMap<u32, Sig>;
    loop {
        rounds += 1;
        loop {
            for va in prog.compiled.clone() {
                let s = prog.summ[&va];
                prog.summ.insert(va, Summary { reads: 0, preserved: s.preserved, pops: s.pops, known: true });
            }
            outs = prog
                .compiled
                .iter()
                .map(|&va| {
                    let g = prog.garbage.get(&va).copied().unwrap_or(0);
                    (va, (set_of(&[EAX, ECX, EDX]) & !g) | (CALLEE_SAVED_REGS & !prog.summ[&va].preserved))
                })
                .collect();
            let mut callers: HashMap<u32, BTreeSet<u32>> = HashMap::new();
            for &va in &prog.compiled {
                let g = prog.func(va);
                for (_, t) in calls_tails(&g) {
                    if let Tgt::Direct(t) = t {
                        if prog.compiled.contains(t) {
                            callers.entry(*t).or_default().insert(va);
                        }
                    }
                }
            }
            let cs: Vec<u32> = prog.compiled.iter().copied().collect();
            let order = prog.callees_first(&cs);
            let mut queued: HashSet<u32> = order.iter().copied().collect();
            let mut work: VecDeque<u32> = order.into_iter().collect();
            let mut budget = 100 * prog.compiled.len().max(1) as i64;
            let mut settled = 0usize;
            let mut hit: OMap<u32, String> = OMap::new();
            let mut put_off: HashSet<u32> = HashSet::new();
            while let Some(va) = work.pop_front() {
                settled += 1;
                progress.tick(
                    &format!("signatures, round {}: {} analysed, {} refused this pass, still queued", rounds, settled, hit.len()),
                    work.len() + 1,
                    0,
                );
                budget -= 1;
                if budget < 0 {
                    panic!("signatures didn't settle");
                }
                queued.remove(&va);
                if hit.contains_key(&va) {
                    continue;
                }
                let f = prog.func(va);
                let res = (|| {
                    let a = Analysis::new(&prog, f.clone(), Some(outs[&va]), true)?;
                    if !a.esp_ok {
                        return Err(Refused(format!("esp at a ret isn't known to be the entry's: {}", a.esp_why())));
                    }
                    Flags::new(&f)?;
                    Ok((a.live_in, a.live_out.clone()))
                })();
                let (live_in, live_out) = match res {
                    Ok(x) => x,
                    Err(e) => {
                        if calls_tails(&f).any(|(_, t)| t.direct().is_some_and(|t| hit.contains_key(&t))) {
                            put_off.insert(va);
                        } else {
                            hit.insert(va, e.0);
                        }
                        continue;
                    }
                };
                put_off.remove(&va);
                let s = prog.summ[&va];
                if live_in != s.reads {
                    prog.summ.insert(va, Summary { reads: live_in, preserved: s.preserved, pops: s.pops, known: true });
                    if let Some(cs) = callers.get(&va) {
                        for &c in cs {
                            if queued.insert(c) {
                                work.push_back(c);
                            }
                        }
                    }
                }
                for (site, t) in calls_tails(&f) {
                    let Tgt::Direct(t) = *t else { continue };
                    if !prog.compiled.contains(&t) {
                        continue;
                    }
                    let live = if f.tails.contains_key(site) { outs[&va] } else { live_out[site] };
                    let g = prog.garbage.get(&t).copied().unwrap_or(0);
                    let add = live & !prog.summ[&t].preserved & !bit(ESP) & !g;
                    if add & !outs[&t] != 0 {
                        *outs.get_mut(&t).unwrap() |= add;
                        if queued.insert(t) {
                            work.push_back(t);
                        }
                    }
                }
            }
            if hit.is_empty() && put_off.is_empty() {
                break;
            }
            if hit.is_empty() {
                panic!("signatures: {} functions put off with no refusal", put_off.len());
            }
            for (va, why) in hit.iter() {
                if why.starts_with("esp at a ret") && prog.make_plain(*va) {
                    continue;
                }
                refused.insert(*va, why.clone());
                prog.compiled.remove(va);
            }
            eprintln!(
                "aot: signatures, round {}: a pass refused {} ({} put off), {:.0} s",
                rounds,
                hit.len(),
                put_off.len(),
                t0.elapsed().as_secs_f64()
            );
            prog.apply_exits(&blocks);
            prog.summarize(&reach_sorted, 50);
        }
        let s: HashMap<u32, Sig> = prog
            .compiled
            .iter()
            .map(|&va| {
                let sm = prog.summ[&va];
                (va, Sig { ins: regs_of(sm.reads & !bit(ESP)), outs: regs_of(outs[&va]), preserved: regs_of(sm.preserved) })
            })
            .collect();
        // Every function emitted once, both ways: what the emitter refuses goes to Box86.
        let mut bad: OMap<u32, String> = OMap::new();
        for &mode in mode_list {
            let mut em = Emitter::new(&prog, mode, &s);
            let cs: Vec<u32> = prog.compiled.iter().copied().collect();
            for (n, &va) in cs.iter().enumerate() {
                progress.tick(&format!("emitter check, round {}, {}", rounds, mode), n, cs.len());
                if bad.contains_key(&va) {
                    continue;
                }
                let r = (|| -> Result<(), Refused> {
                    let out = set_of(&s[&va].outs);
                    em.ana = Some(Analysis::new(&prog, prog.func(va), Some(out), mode == "sig")?);
                    em.function(va)?;
                    Ok(())
                })();
                if let Err(e) = r {
                    bad.insert(va, e.0);
                }
            }
        }
        if bad.is_empty() {
            sigs = s;
            break;
        }
        for (va, why) in bad.iter() {
            refused.insert(*va, why.clone());
            prog.compiled.remove(va);
        }
        prog.apply_exits(&blocks);
        prog.summarize(&reach_sorted, 50);
        eprintln!("aot: round {}: {} refused by the emitter, {:.0} s", rounds, bad.len(), t0.elapsed().as_secs_f64());
    }
    let t1 = Instant::now();
    write_out(img, prog, sigs, refused, roots, mode_list, outdir, names, reach.len(), rounds, t0, t1)
}

const CALLEE_SAVED_REGS: Regs = (1 << EBX) | (1 << EBP) | (1 << ESI) | (1 << EDI);

#[allow(clippy::too_many_arguments)]
fn write_out<'a>(
    img: &'a Image,
    prog: Program<'a>,
    sigs: HashMap<u32, Sig>,
    refused: OMap<u32, String>,
    roots: &[u32],
    mode_list: &[&'static str],
    outdir: &str,
    names: &HashMap<u32, String>,
    closure: usize,
    rounds: usize,
    t0: Instant,
    t1: Instant,
) -> Compiled<'a> {
    let mut progress = Progress::new(t0);
    let head: Vec<String> = vec![
        "/* Generated by tools/aot/recompile from Game.exe: never commit it (docs/aot.md). */".to_string(),
        "#include \"game/aot_rt.h\"".to_string(),
        String::new(),
    ];
    let mut chunks: Vec<String> = head.clone();
    let mut report = J::obj();
    report.set("functions", J::obj());
    report.set("refused", J::Obj(refused.iter().map(|(k, v)| (format!("{:08x}", k), J::Str(v.clone()))).collect()));
    report.set("fallbacks", J::obj());
    let compiled: Vec<u32> = prog.compiled.iter().copied().collect();
    let mut texts: HashMap<u32, Vec<(&'static str, String)>> = HashMap::new();
    let mut ems: Vec<Emitter> = Vec::new();
    for &mode in mode_list {
        let mut em = Emitter::new(&prog, mode, &sigs);
        chunks.extend(compiled.iter().map(|&va| em.proto(va) + ";"));
        chunks.push(String::new());
        for (n, &va) in compiled.iter().enumerate() {
            progress.tick(&format!("writing the C, {}", mode), n, compiled.len());
            let out = set_of(&sigs[&va].outs);
            em.ana = Some(Analysis::new(&prog, prog.func(va), Some(out), mode == "sig").expect("analysis passed before"));
            let text = em.function(va).expect("emitted before");
            texts.entry(va).or_default().push((mode, text));
        }
        em.ana = None;
        ems.push(em);
    }
    let part_max: usize = (std::env::var("AOT_PART_MB").ok().and_then(|s| s.parse::<f64>().ok()).unwrap_or(8.0) * 1e6) as usize;
    let cset: HashSet<u32> = compiled.iter().copied().collect();
    let callees_of: HashMap<u32, BTreeSet<u32>> = compiled
        .iter()
        .map(|&va| {
            let f = prog.func(va);
            (va, calls_tails(&f).filter_map(|(_, t)| t.direct()).filter(|t| cset.contains(t)).collect())
        })
        .collect();
    let size_of = |va: u32| -> usize { texts[&va].iter().map(|(_, t)| t.len()).sum() };
    let cut = |order: &[u32]| -> (usize, Vec<Vec<u32>>) {
        let mut parts: Vec<Vec<u32>> = Vec::new();
        let mut cur: Vec<u32> = Vec::new();
        let mut size = 0usize;
        for &va in order {
            let n = size_of(va);
            if !cur.is_empty() && size + n > part_max {
                parts.push(std::mem::take(&mut cur));
                size = 0;
            }
            cur.push(va);
            size += n;
        }
        if !cur.is_empty() {
            parts.push(cur);
        }
        let mut wh: HashMap<u32, usize> = HashMap::new();
        for (k, p) in parts.iter().enumerate() {
            for &va in p {
                wh.insert(va, k);
            }
        }
        let across = compiled.iter().map(|va| callees_of[va].iter().filter(|t| wh[t] != wh[va]).count()).sum();
        (across, parts)
    };
    let clusters = || -> Vec<u32> {
        let mut w: BTreeMap<(u32, u32), i64> = BTreeMap::new();
        for &va in &compiled {
            let g = prog.func(va);
            for (_, t) in calls_tails(&g) {
                if let Tgt::Direct(t) = *t {
                    if cset.contains(&t) && t != va {
                        *w.entry((va.min(t), va.max(t))).or_insert(0) += 1;
                    }
                }
            }
        }
        let mut up: HashMap<u32, u32> = compiled.iter().map(|&v| (v, v)).collect();
        let mut csize: HashMap<u32, usize> = compiled.iter().map(|&v| (v, size_of(v))).collect();
        let mut members: HashMap<u32, Vec<u32>> = compiled.iter().map(|&v| (v, vec![v])).collect();
        fn find(up: &mut HashMap<u32, u32>, mut x: u32) -> u32 {
            while up[&x] != x {
                let g = up[&up[&x]];
                up.insert(x, g);
                x = g;
            }
            x
        }
        let mut pairs: Vec<((u32, u32), i64)> = w.into_iter().collect();
        pairs.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
        for ((a, b), _) in pairs {
            let (mut ra, mut rb) = (find(&mut up, a), find(&mut up, b));
            if ra == rb || csize[&ra] + csize[&rb] > part_max {
                continue;
            }
            if csize[&ra] < csize[&rb] {
                std::mem::swap(&mut ra, &mut rb);
            }
            up.insert(rb, ra);
            let sb = csize.remove(&rb).unwrap();
            *csize.get_mut(&ra).unwrap() += sb;
            let mb = members.remove(&rb).unwrap();
            members.get_mut(&ra).unwrap().extend(mb);
        }
        let mut rs: Vec<u32> = members.keys().copied().collect();
        rs.sort_by(|a, b| csize[b].cmp(&csize[a]).then(a.cmp(b)));
        let mut bins: Vec<(usize, Vec<u32>)> = Vec::new();
        for r in rs {
            let mut placed = false;
            for b in bins.iter_mut() {
                if b.0 + csize[&r] <= part_max {
                    b.0 += csize[&r];
                    b.1.extend(members[&r].iter().copied());
                    placed = true;
                    break;
                }
            }
            if !placed {
                bins.push((csize[&r], members[&r].clone()));
            }
        }
        let mut order = Vec::new();
        for (_, mut b) in bins {
            b.sort();
            order.extend(b);
        }
        order
    };
    let cands: Vec<(&str, (usize, Vec<Vec<u32>>))> =
        vec![("address", cut(&compiled)), ("post-order", cut(&prog.callees_first(&compiled))), ("clusters", cut(&clusters()))];
    let mut best = 0;
    for k in 1..cands.len() {
        let (a, b) = (&cands[k].1, &cands[best].1);
        if (a.0, a.1.len()) < (b.0, b.1.len()) {
            best = k;
        }
    }
    eprintln!(
        "aot: calls across parts by order: {}",
        cands.iter().map(|(k, v)| format!("{} {} ({} parts)", k, v.0, v.1.len())).collect::<Vec<_>>().join(", ")
    );
    let order_name = cands[best].0;
    let parts: Vec<Vec<u32>> = cands[best]
        .1
         .1
        .iter()
        .map(|p| {
            let mut p = p.clone();
            p.sort();
            p
        })
        .collect();
    let mut part_of: HashMap<u32, usize> = HashMap::new();
    for (k, p) in parts.iter().enumerate() {
        for &va in p {
            part_of.insert(va, k);
        }
    }
    let (mut cross, mut local) = (0i64, 0i64);
    let mut part_texts: Vec<Vec<String>> = Vec::new();
    for (k, part) in parts.iter().enumerate() {
        let mut out = head.clone();
        let mut callees: BTreeSet<u32> = BTreeSet::new();
        for &va in part {
            let g = prog.func(va);
            for (_, t) in calls_tails(&g) {
                if let Tgt::Direct(t) = *t {
                    if cset.contains(&t) {
                        callees.insert(t);
                        if part_of[&t] == k {
                            local += 1;
                        } else {
                            cross += 1;
                        }
                    }
                }
            }
        }
        let mut need: BTreeSet<u32> = part.iter().copied().collect();
        need.extend(callees.iter().copied());
        for (mi, &mode) in mode_list.iter().enumerate() {
            let em = &ems[mi];
            out.extend(need.iter().map(|&va| em.proto(va) + ";"));
            out.push(String::new());
            if mode == "sig" {
                out.extend(callees.iter().map(|&va| em.wrappers(va)));
                out.push(String::new());
            }
            for &va in part {
                out.push(texts[&va].iter().find(|(m, _)| *m == mode).unwrap().1.clone());
                out.push(String::new());
            }
        }
        part_texts.push(out);
    }
    let mut pj = J::obj();
    pj.set("files", J::Int(parts.len() as i64));
    pj.set("calls_local", J::Int(local));
    pj.set("calls_across", J::Int(cross));
    pj.set("order", J::Str(order_name.to_string()));
    report.set("parts", pj);
    for (mi, &mode) in mode_list.iter().enumerate() {
        let em = &ems[mi];
        for &va in roots {
            if !cset.contains(&va) {
                continue;
            }
            let mut body: Vec<String> = Vec::new();
            if mode == "ctx" {
                body.push(format!("AotRegs t; for (int k = 0; k < 8; k++) t.r[k] = r[k]; {}(&t);", em.name(va)));
                body.push("for (int k = 0; k < 8; k++) if (k != 4) r[k] = t.r[k];".to_string());
            } else {
                let sig = &sigs[&va];
                let mut a = vec!["r[4]".to_string()];
                a.extend(sig.ins.iter().map(|x| format!("r[{}]", x)));
                let args = a.join(", ");
                let o = &sig.outs;
                if o.len() == 1 {
                    body.push(format!("r[{}] = {}({});", o[0], em.name(va), args));
                } else if o.len() == 2 {
                    body.push(format!("uint64_t t = {}({}); r[{}] = (uint32_t)t; r[{}] = (uint32_t)(t >> 32);", em.name(va), args, o[0], o[1]));
                } else {
                    body.push(format!("uint32_t o[8]; {}({}{});", em.name(va), args, if o.is_empty() { "" } else { ", o" }));
                    body.push(o.iter().enumerate().map(|(k, x)| format!("r[{}] = o[{}];", x, k)).collect::<Vec<_>>().join(" "));
                }
            }
            chunks.push(format!(
                "static uint32_t AOT_SYM({}_root_{:08x})(uint32_t *r)\n{{\n    {}\n    return r[0];\n}}\n",
                mode,
                va,
                body.join("\n    ")
            ));
        }
    }
    let (heads, why) = hookable(img, &prog);
    let mut leaf_n = 0usize;
    if mode_list.contains(&"sig") {
        let em = Emitter::new(&prog, "sig", &sigs);
        for &va in &compiled {
            let sig = &sigs[&va];
            let mut a = vec!["r[4]".to_string()];
            a.extend(sig.ins.iter().map(|x| format!("r[{}]", x)));
            let args = a.join(", ");
            let o = &sig.outs;
            let body = if o.len() == 1 {
                format!("r[{}] = {}({});", o[0], em.name(va), args)
            } else if o.len() == 2 {
                format!("uint64_t t = {}({}); r[{}] = (uint32_t)t; r[{}] = (uint32_t)(t >> 32);", em.name(va), args, o[0], o[1])
            } else {
                format!(
                    "uint32_t o[8]; {}({}{}); {}",
                    em.name(va),
                    args,
                    if o.is_empty() { "" } else { ", o" },
                    o.iter().enumerate().map(|(k, x)| format!("r[{}] = o[{}];", x, k)).collect::<Vec<_>>().join(" ")
                )
            };
            chunks.push(format!("static uint32_t AOT_SYM(fn_{:08x})(uint32_t *r) {{ {} return r[0]; }}", va, body));
        }
        let index: HashMap<u32, usize> = compiled.iter().enumerate().map(|(k, &v)| (v, k)).collect();
        for &va in &compiled {
            let f = prog.func(va);
            let rg = code_ranges(&f);
            let cl: BTreeSet<usize> = calls_tails(&f).filter_map(|(_, t)| t.direct()).filter(|t| cset.contains(t)).map(|t| index[&t]).collect();
            chunks.push(format!(
                "static const uint32_t AOT_SYM(rg_{:08x})[] = {{{}}};",
                va,
                rg.iter().map(|(lo, hi)| format!("0x{:08x}u, 0x{:08x}u", lo, hi)).collect::<Vec<_>>().join(", ")
            ));
            if !cl.is_empty() {
                chunks.push(format!(
                    "static const uint16_t AOT_SYM(cl_{:08x})[] = {{{}}};",
                    va,
                    cl.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(", ")
                ));
            }
        }
        let leaf = leaves(&prog, &prog.compiled);
        leaf_n = leaf.len();
        report.set("leaves", J::Int(leaf.len() as i64));
        report.set("x87", J::Int(compiled.iter().filter(|&&va| prog.func(va).insns.values().any(|i| x87_op(i).is_some())).count() as i64));
        let mut ex = J::obj();
        ex.set("functions", J::Int(compiled.iter().filter(|&&va| !prog.func(va).exits.is_empty()).count() as i64));
        ex.set("sites", J::Int(compiled.iter().map(|&va| prog.func(va).exits.len() as i64).sum()));
        ex.set("may_exit", J::Int(prog.may_exit.iter().filter(|v| cset.contains(v)).count() as i64));
        ex.set("callouts", J::Int(compiled.iter().map(|&va| prog.func(va).callouts.len() as i64).sum()));
        ex.set("plain", J::Int(prog.plain.iter().filter(|v| cset.contains(v)).count() as i64));
        report.set("exits", ex);
        chunks.push("const AotFunc AOT_SYM(funcs)[] = {".to_string());
        for &va in &compiled {
            let (keep, hd) = heads.get(&va).cloned().unwrap_or((0, vec![0u8; 16]));
            let f = prog.func(va);
            let rg = code_ranges(&f);
            let ncl = calls_tails(&f).filter_map(|(_, t)| t.direct()).filter(|t| cset.contains(t)).collect::<HashSet<_>>().len();
            let name = names.get(&va).cloned().unwrap_or_else(|| format!("{:08x}", va));
            let hard = f.exits.iter().any(|s| !f.callouts.contains_key(s));
            chunks.push(format!(
                "    {{0x{:08x}u, AOT_SYM(fn_{:08x}), \"{}\", {}, {}, 0x{:02x}, 0x{:02x}, {}, {{{}}}, 0x{:08x}u, {}, {}, AOT_SYM(rg_{:08x}), {}, AOT_LEAF({}), AOT_EXIT({}, {}), AOT_EXIT({}, {}), {}}},",
                va,
                va,
                name,
                keep,
                prog.summ[&va].pops.unwrap_or(0),
                mask(&sigs[&va].outs),
                mask(&sigs[&va].preserved),
                if roots.contains(&va) { 1 } else { 0 },
                hd.iter().map(|b| format!("0x{:02x}", b)).collect::<Vec<_>>().join(", "),
                code_hash(img, &rg),
                rg.len(),
                ncl,
                va,
                if ncl > 0 { format!("AOT_SYM(cl_{:08x})", va) } else { "0".to_string() },
                if leaf.contains(&va) { 1 } else { 0 },
                if f.exits.is_empty() { 0 } else { 1 },
                if prog.may_exit.contains(&va) { 1 } else { 0 },
                if hard { 1 } else { 0 },
                if prog.may_hard.contains(&va) { 1 } else { 0 },
                if f.insns.values().any(|i| x87_op(i).is_some()) { 1 } else { 0 }
            ));
        }
        chunks.push("    {0, 0, 0, 0, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0}".to_string());
        chunks.push("};".to_string());
    }
    let mut hk = J::obj();
    hk.set("hookable", J::Int(heads.len() as i64));
    hk.set("not", J::Obj(why.iter().map(|(k, v)| (format!("{:08x}", k), J::Str(v.clone()))).collect()));
    report.set("hooks", hk);
    chunks.push("const AotRoot AOT_SYM(roots)[] = {".to_string());
    for &va in roots {
        if cset.contains(&va) {
            let (keep, hd) = root_head(img, va).expect("a root's head");
            let name = names.get(&va).cloned().unwrap_or_else(|| format!("{:08x}", va));
            chunks.push(format!(
                "    {{0x{:08x}u, {}, {}, \"{}\", {}, {}, {{{}}}}},",
                va,
                if mode_list.contains(&"sig") { format!("AOT_SYM(sig_root_{:08x})", va) } else { "0".to_string() },
                if mode_list.contains(&"ctx") { format!("AOT_SYM(ctx_root_{:08x})", va) } else { "0".to_string() },
                name,
                keep,
                prog.summ[&va].pops.expect("a root's pops"),
                hd.iter().map(|b| format!("0x{:02x}", b)).collect::<Vec<_>>().join(", ")
            ));
        }
    }
    chunks.push("    {0, 0, 0, 0, 0, 0, {0}}".to_string());
    chunks.push("};".to_string());
    std::fs::create_dir_all(outdir).expect("the output directory");
    if let Ok(rd) = std::fs::read_dir(outdir) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if let Some(mid) = n.strip_prefix("aot_spike_").and_then(|x| x.strip_suffix(".c")) {
                if !mid.is_empty() && mid.chars().all(|c| c.is_ascii_digit()) {
                    let _ = std::fs::remove_file(e.path());
                }
            }
        }
    }
    let p = std::path::Path::new(outdir);
    std::fs::write(p.join("aot_spike.c"), chunks.join("\n") + "\n").expect("aot_spike.c");
    for (k, out) in part_texts.iter().enumerate() {
        std::fs::write(p.join(format!("aot_spike_{:02}.c", k + 1)), out.join("\n") + "\n").expect("a part");
    }
    let mut insns = 0usize;
    let mut fj = Vec::new();
    for &va in &compiled {
        let f = prog.func(va);
        insns += f.n_insns;
        let falls: BTreeSet<u32> = calls_tails(&f)
            .filter_map(|(_, t)| t.direct())
            .filter(|t| !cset.contains(t) && !crate::program::intrinsics().contains_key(t))
            .collect();
        let nat = natives();
        let mut o = J::obj();
        o.set("insns", J::Int(f.n_insns as i64));
        o.set("bytes", J::Int(f.n_bytes as i64));
        let mut sj = J::obj();
        sj.set("in", int_list(&sigs[&va].ins));
        sj.set("out", int_list(&sigs[&va].outs));
        sj.set("preserved", int_list(&sigs[&va].preserved));
        o.set("sig", sj);
        o.set("ranges", J::List(code_ranges(&f).iter().map(|(a, b)| J::List(vec![J::Int(*a as i64), J::Int(*b as i64)])).collect()));
        o.set("switches", J::Int(f.switches.len() as i64));
        o.set("natives", J::List(falls.iter().filter(|t| nat.contains_key(t)).map(|t| J::Str(format!("{:08x}", t))).collect()));
        o.set("box86", J::List(falls.iter().filter(|t| !nat.contains_key(t)).map(|t| J::Str(format!("{:08x}", t))).collect()));
        fj.push((format!("{:08x}", va), o));
    }
    report.set("functions", J::Obj(fj));
    let mut trees = J::obj();
    for &r in roots {
        let mut seen: BTreeSet<u32> = BTreeSet::new();
        let mut work = vec![r];
        while let Some(v) = work.pop() {
            if seen.contains(&v) || !cset.contains(&v) {
                continue;
            }
            seen.insert(v);
            let g = prog.func(v);
            work.extend(calls_tails(&g).filter_map(|(_, t)| t.direct()));
        }
        trees.set(&format!("{:08x}", r), J::List(seen.iter().map(|v| J::Str(format!("{:08x}", v))).collect()));
    }
    report.set("trees", trees);
    let x86_bytes: u64 = compiled.iter().map(|&va| prog.func(va).n_bytes).sum();
    report.set("x86_insns", J::Int(insns as i64));
    report.set("x86_bytes", J::Int(x86_bytes as i64));
    let analysis_s = round3((t1 - t0).as_secs_f64());
    let emit_s = round3(t1.elapsed().as_secs_f64());
    report.set("analysis_s", J::Float(analysis_s));
    report.set("rounds", J::Int(rounds as i64));
    report.set("emit_s", J::Float(emit_s));
    report.set("closure", J::Int(closure as i64));
    std::fs::write(p.join("aot_spike.json"), report.dump()).expect("aot_spike.json");
    drop(ems);
    Compiled { prog, report, refused, x86_insns: insns, x86_bytes, leaves: leaf_n, rounds, analysis_s, emit_s }
}
