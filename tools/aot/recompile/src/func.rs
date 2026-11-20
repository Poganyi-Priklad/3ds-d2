//! One function's code: its instructions and basic blocks from its entry
//! (recompile.py's Func).

use crate::cs::{reg_full, Insn, OpKind};
use crate::image::Image;
use crate::omap::OMap;
use crate::x86::regs_rw;
use crate::Refused;
use std::collections::{BTreeSet, HashSet};
use std::rc::Rc;

/// A call's or tail jump's target: a function's entry, an import's IAT
/// slot, or through a pointer (None in recompile.py).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, PartialOrd, Ord)]
pub enum Tgt {
    Direct(u32),
    Import(u32),
    Indirect,
}

impl Tgt {
    pub fn direct(self) -> Option<u32> {
        match self {
            Tgt::Direct(v) => Some(v),
            _ => None,
        }
    }
    /// None or an import (recompile.py's `t is None or isinstance(t, tuple)`).
    pub fn is_pointer(self) -> bool {
        !matches!(self, Tgt::Direct(_))
    }
}

/// A switch's jmp: the index register, the targets, and (kept as
/// recompile.py has them) the byte table, the default and the bound.
#[derive(Clone, Debug)]
#[allow(dead_code)]
pub struct Switch {
    pub idx: u8,
    pub targets: Vec<u32>,
    pub bytes_table: Option<u32>,
    pub default: u32,
    pub bound: i64,
}

pub const NORETURN_IMPORTS: [&str; 5] = ["ExitProcess", "ExitThread", "FatalAppExitA", "FatalExit", "TerminateThread"];

#[derive(Clone)]
pub struct Func {
    pub entry: u32,
    pub insns: OMap<u32, Rc<Insn>>,
    pub succ: OMap<u32, Vec<u32>>,
    pub calls: OMap<u32, Tgt>,
    pub tails: OMap<u32, Tgt>,
    pub switches: OMap<u32, Switch>,
    pub rets: OMap<u32, i64>,
    pub noreturn: HashSet<u32>,
    pub exits: BTreeSet<u32>,
    pub callouts: OMap<u32, i64>,
    pub plain: bool,
    pub data: HashSet<u32>,
    pub refused: Option<String>,
    pub ret_pops: BTreeSet<i64>,
    pub tail_targets: BTreeSet<Tgt>,
    pub leaders: HashSet<u32>,
    pub order: Vec<u32>,
    pub n_insns: usize,
    pub n_bytes: u64,
}

fn imm_addr(v: i64) -> u32 {
    v as u32
}

impl Func {
    pub fn new(img: &Image, entry: u32, starts: &HashSet<u32>) -> Func {
        let mut f = Func {
            entry,
            insns: OMap::new(),
            succ: OMap::new(),
            calls: OMap::new(),
            tails: OMap::new(),
            switches: OMap::new(),
            rets: OMap::new(),
            noreturn: HashSet::new(),
            exits: BTreeSet::new(),
            callouts: OMap::new(),
            plain: false,
            data: HashSet::new(),
            refused: None,
            ret_pops: BTreeSet::new(),
            tail_targets: BTreeSet::new(),
            leaders: HashSet::new(),
            order: Vec::new(),
            n_insns: 0,
            n_bytes: 0,
        };
        if let Err(e) = f.walk(img, starts) {
            f.refused = Some(e.0);
        }
        f.ret_pops = f.rets.values().copied().collect();
        f.tail_targets = f.tails.values().copied().collect();
        f
    }

    fn walk(&mut self, img: &Image, starts: &HashSet<u32>) -> Result<(), Refused> {
        let mut work = vec![self.entry];
        while let Some(mut va) = work.pop() {
            while !self.insns.contains_key(&va) {
                let i = img.insn(va)?;
                self.insns.insert(va, i.clone());
                let m = i.mnemonic.as_str();
                let nxt = va.wrapping_add(i.size);
                let ops = &i.ops;
                if m == "ret" {
                    self.rets.insert(va, if ops.is_empty() { 0 } else { ops[0].imm() });
                    self.succ.insert(va, vec![]);
                    break;
                }
                if m == "int3" || m == "hlt" || m == "ud2" {
                    self.succ.insert(va, vec![]);
                    break;
                }
                if m == "jmp" {
                    if ops[0].is_imm() {
                        let t = imm_addr(ops[0].imm());
                        if t != self.entry && starts.contains(&t) {
                            self.tails.insert(va, Tgt::Direct(t));
                            self.succ.insert(va, vec![]);
                        } else {
                            self.succ.insert(va, vec![t]);
                            work.push(t);
                        }
                        break;
                    }
                    match self.switch(img, va, &i)? {
                        None => {
                            // Not through a table: a tail call through a pointer (issue #138).
                            let op = ops[0];
                            let mm = op.mem();
                            let imp = op.is_mem()
                                && mm.base == 0
                                && mm.index == 0
                                && (0..=u32::MAX as i64).contains(&mm.disp)
                                && img.imports.contains_key(&(mm.disp as u32));
                            self.tails.insert(va, if imp { Tgt::Import(mm.disp as u32) } else { Tgt::Indirect });
                            self.succ.insert(va, vec![]);
                        }
                        Some(sw) => {
                            // Not the guard's default: the ja before goes there, and the
                            // index here is in the table (issue #138: the jmp's pushes
                            // reached a default that is the ret, two esps there).
                            let mut s: Vec<u32> = sw.targets.clone();
                            s.sort();
                            s.dedup();
                            work.extend(s.iter().copied());
                            self.succ.insert(va, s);
                            self.switches.insert(va, sw);
                        }
                    }
                    break;
                }
                if m.starts_with('j') || m.starts_with("loop") {
                    if !ops[0].is_imm() {
                        return Err(Refused(format!("indirect conditional jump at {:08x}", va)));
                    }
                    let t = imm_addr(ops[0].imm());
                    self.succ.insert(va, vec![t, nxt]);
                    work.push(t);
                    va = nxt;
                    continue;
                }
                if m == "call" {
                    let op = ops[0];
                    let mm = op.mem();
                    let t = if op.is_imm() && img.native_slots.contains(&imm_addr(op.imm())) {
                        Tgt::Import(imm_addr(op.imm()))   // a native's site (issue #138): its call as an import's
                    } else if op.is_imm() {
                        Tgt::Direct(imm_addr(op.imm()))
                    } else if op.is_mem()
                        && mm.base == 0
                        && mm.index == 0
                        && (0..=u32::MAX as i64).contains(&mm.disp)
                        && img.imports.contains_key(&(mm.disp as u32))
                    {
                        Tgt::Import(mm.disp as u32)
                    } else {
                        Tgt::Indirect
                    };
                    self.calls.insert(va, t);
                    if let Tgt::Import(slot) = t {
                        if NORETURN_IMPORTS.contains(&img.imports[&slot].1.as_str()) {
                            self.noreturn.insert(va);
                            self.succ.insert(va, vec![]);
                            break;
                        }
                    }
                    let dead = match img.insn(nxt) {
                        Ok(n) => n.mnemonic == "int3" || (starts.contains(&nxt) && nxt != self.entry),
                        Err(_) => true,
                    };
                    if dead {
                        self.noreturn.insert(va);
                        self.succ.insert(va, vec![]);
                        break;
                    }
                    self.succ.insert(va, vec![nxt]);
                    va = nxt;
                    continue;
                }
                self.succ.insert(va, vec![nxt]);
                va = nxt;
            }
        }
        for va in self.insns.keys() {
            for t in &self.succ[va] {
                if !self.insns.contains_key(t) {
                    return Err(Refused(format!("branch into the middle of an instruction at {:08x}", t)));
                }
            }
        }
        let mut data: Vec<u32> = self.data.iter().copied().collect();
        data.sort();
        for d in data {
            if self.insns.contains_key(&d) {
                return Err(Refused(format!("a switch table overlaps code at {:08x}", d)));
            }
        }
        self.blocks();
        Ok(())
    }

    /// Calls to functions that never return end their path.
    pub fn prune(&mut self, noreturn_funcs: &HashSet<u32>) -> bool {
        let hit: Vec<u32> = self
            .calls
            .iter()
            .filter(|(va, t)| t.direct().is_some_and(|d| noreturn_funcs.contains(&d)) && !self.noreturn.contains(va))
            .map(|(va, _)| *va)
            .collect();
        if hit.is_empty() {
            return false;
        }
        self.cut(&hit);
        true
    }

    /// The exits Box86 runs on from (issue #115); indirect and import calls
    /// callouts, and tail calls through a pointer where the pops are known.
    pub fn set_exits(&mut self, sites: &BTreeSet<u32>, callouts: bool, tail_pops: Option<i64>, img: &Image, enabled: bool) {
        self.exits = sites.clone();
        self.plain = !callouts;
        if enabled && callouts {
            let mut co = OMap::new();
            for &va in sites {
                if let Some(t) = self.calls.get(&va) {
                    if !self.noreturn.contains(&va) && t.is_pointer() {
                        co.insert(va, self.callout_pops(va, img));
                    }
                }
            }
            self.callouts = co;
            let pops = if self.ret_pops.len() == 1 {
                self.ret_pops.iter().next().copied()
            } else if self.ret_pops.is_empty() {
                tail_pops
            } else {
                None
            };
            if let Some(p) = pops {
                for &va in sites {
                    if let Some(t) = self.tails.get(&va) {
                        if t.is_pointer() {
                            self.callouts.insert(va, p);
                        }
                    }
                }
            }
        }
        let hit: Vec<u32> = sites
            .iter()
            .copied()
            .filter(|va| self.calls.contains_key(va) && !self.noreturn.contains(va) && !self.callouts.contains_key(va))
            .collect();
        self.cut(&hit);
    }

    /// What a callout's callee is taken to pop: an import's, none where the
    /// caller cleans up (add esp after), else the pushes just before it.
    pub fn callout_pops(&self, va: u32, img: &Image) -> i64 {
        if let Some(Tgt::Import(slot)) = self.calls.get(&va) {
            if let Some(Some(p)) = img.import_pops.get(slot) {
                return *p;
            }
        }
        let i = &self.insns[&va];
        if let Some(n) = self.insns.get(&va.wrapping_add(i.size)) {
            if n.mnemonic == "add" && n.op_str.starts_with("esp, ") {
                return 0;
            }
        }
        let mut ends = std::collections::HashMap::new();
        for (k, j) in self.insns.iter() {
            ends.insert(k.wrapping_add(j.size), *k);
        }
        let (mut n, mut cur) = (0i64, va);
        while !self.leaders.contains(&cur) {
            let Some(&k) = ends.get(&cur) else { break };
            let j = &self.insns[&k];
            if j.mnemonic == "push" {
                n += 1;
            } else if matches!(j.mnemonic.as_str(), "call" | "pop" | "ret" | "leave" | "pushal" | "popal")
                || j.op_str.split(',').next().unwrap_or("").trim() == "esp"
                // A frame's mov ebp, esp (issue #138: renderer_line's push ebp
                // counted as an argument, 20 for 16, so its calls were exits).
                || j.op_str.split(',').nth(1).is_some_and(|o| o.trim() == "esp")
            {
                break;
            }
            cur = k;
        }
        4 * n
    }

    pub fn cut(&mut self, hit: &[u32]) {
        for &va in hit {
            self.noreturn.insert(va);
            self.succ.insert(va, vec![]);
        }
        let mut seen = HashSet::new();
        let mut work = vec![self.entry];
        while let Some(va) = work.pop() {
            if !seen.insert(va) {
                continue;
            }
            if let Some(s) = self.succ.get(&va) {
                work.extend(s.iter().copied());
            }
        }
        self.insns.retain(|k| seen.contains(k));
        self.succ.retain(|k| seen.contains(k));
        self.calls.retain(|k| seen.contains(k));
        self.rets.retain(|k| seen.contains(k));
        self.switches.retain(|k| seen.contains(k));
        self.tails.retain(|k| seen.contains(k));
        self.noreturn.retain(|k| seen.contains(k));
        self.blocks();
    }

    fn blocks(&mut self) {
        let mut leaders = HashSet::new();
        leaders.insert(self.entry);
        for (va, s) in self.succ.iter() {
            let i = &self.insns[va];
            let nxt = va.wrapping_add(i.size);
            if s.len() != 1 || s[0] != nxt {
                leaders.extend(s.iter().copied());
                if self.insns.contains_key(&nxt) {
                    leaders.insert(nxt);
                }
            }
        }
        self.leaders = leaders;
        let mut order: Vec<u32> = self.insns.keys().copied().collect();
        order.sort();
        self.order = order;
        self.n_insns = self.insns.len();
        self.n_bytes = self.insns.values().map(|i| i.size as u64).sum();
    }

    /// jmp [reg*4 + table], bounded by the cmp/ja that guards the index; or
    /// through a byte index table first. None when it isn't through a table.
    fn switch(&mut self, img: &Image, va: u32, i: &Insn) -> Result<Option<Switch>, Refused> {
        let op = i.ops[0];
        let OpKind::Mem(m) = op.kind else { return Ok(None) };
        if m.base != 0 || m.scale != 4 || m.index == 0 {
            return Ok(None);
        }
        let idx = reg_full(m.index).expect("index register").0;
        let table = m.disp as u32;
        let mut back: Vec<u32> = self.insns.keys().copied().filter(|&a| a < va).collect();
        back.sort();
        let mut bytes_table: Option<u32> = None;
        let mut bound: Option<i64> = None;
        let mut default: Option<i64> = None;
        let mut cur = idx;
        let mut k = back.len() as i64 - 1;
        while k >= 0 && bound.is_none() {
            let a = back[k as usize];
            let p = self.insns[&a].clone();
            let next = if ((k + 1) as usize) < back.len() { back[(k + 1) as usize] } else { va };
            if a.wrapping_add(p.size) != next {
                break;
            }
            let ops = &p.ops;
            let mn = p.mnemonic.as_str();
            if mn == "movzx"
                && ops[0].is_reg()
                && reg_full(ops[0].reg()).map(|s| s.0) == Some(cur)
                && ops[1].is_mem()
                && ops[1].size == 1
                && ops[1].mem().index == 0
                && ops[1].mem().base != 0
            {
                bytes_table = Some(ops[1].mem().disp as u32);
                cur = reg_full(ops[1].mem().base).expect("base register").0;
            } else if mn == "ja" && default.is_none() {
                default = Some(ops[0].imm());
            } else if mn == "cmp"
                && default.is_some()
                && ops[0].is_reg()
                && reg_full(ops[0].reg()).map(|s| s.0) == Some(cur)
                && ops[1].is_imm()
            {
                bound = Some(ops[1].imm());
            } else {
                let (_, w) = regs_rw(&p);
                if w & (1 << cur) != 0 || (bytes_table.is_none() && w & (1 << idx) != 0) {
                    break;
                }
            }
            k -= 1;
        }
        let (Some(bound), Some(default)) = (bound, default) else {
            return Err(Refused(format!("a switch at {:08x} the guard rule doesn't bound", va)));
        };
        if bound > 512 {
            return Err(Refused(format!("a switch at {:08x} the guard rule doesn't bound", va)));
        }
        let n = if let Some(bt) = bytes_table {
            let mut mx = 0u32;
            for j in 0..=bound {
                mx = mx.max(img.u8(bt.wrapping_add(j as u32)) as u32);
            }
            for j in 0..=bound {
                self.data.insert(bt.wrapping_add(j as u32));
            }
            1 + mx as i64
        } else {
            bound + 1
        };
        let targets: Vec<u32> = (0..n).map(|j| img.u32(table.wrapping_add(4 * j as u32))).collect();
        for j in 0..4 * n {
            self.data.insert(table.wrapping_add(j as u32));
        }
        for &t in &targets {
            if !img.in_text(t) {
                return Err(Refused(format!("a switch target outside .text at {:08x}", va)));
            }
        }
        Ok(Some(Switch { idx, targets, bytes_table, default: default as u32, bound }))
    }
}
