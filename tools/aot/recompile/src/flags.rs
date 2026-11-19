//! Which instruction's flags each reader reads, and which definers store
//! their operands for it (recompile.py's Flags).

use crate::func::Func;
use crate::x86::{flags_rw, kind, FALL};
use crate::Refused;
use std::collections::{BTreeSet, HashMap, HashSet};

const ENTRY: i64 = -1;

/// A set of definers (ENTRY for the function's entry).
type Defs = BTreeSet<i64>;

pub struct Flags {
    /// reader -> (its definers in order, their (kind, width) where they agree)
    pub readers: HashMap<u32, (Vec<i64>, Option<(u8, u8)>)>,
    pub live_defs: HashSet<u32>,
    pub dynamic: HashSet<u32>,
}

/// (kind, width) of a definer's flags, as aot_rt.h numbers them.
pub fn kind_of(f: &Func, va: u32) -> Option<(u8, u8)> {
    let i = &f.insns[&va];
    let m = i.mnemonic.as_str();
    let k = kind(m)?;
    if matches!(m, "shl" | "sal" | "shr" | "sar") && !i.ops[1].is_imm() {
        return None;
    }
    if (m == "imul" || m == "mul") && i.ops.len() == 1 && i.ops[0].size != 4 {
        return None;
    }
    Some((k, i.ops[0].size))
}

fn need_reader(m: &str, va: u32) -> Result<(), Refused> {
    if m.starts_with('j') || m.starts_with("set") || m.starts_with("cmov") || m == "adc" || m == "sbb" {
        return Ok(());
    }
    Err(Refused(format!("{} reads flags at {:08x}", m, va)))
}

impl Flags {
    pub fn new(f: &Func) -> Result<Flags, Refused> {
        let mut defs: HashMap<u32, u8> = HashMap::new();
        let mut uses: Vec<(u32, u8)> = Vec::new();
        for (va, i) in f.insns.iter() {
            let (mut d, mut u) = flags_rw(i);
            if f.calls.contains_key(va) {
                d = FALL;
                u = 0;
            }
            defs.insert(*va, d);
            uses.push((*va, u));
            if u != 0 {
                need_reader(&i.mnemonic, *va)?;
            }
        }
        // Reaching definitions per flag bit.
        let entry_st: [Defs; 6] = std::array::from_fn(|_| [ENTRY].into_iter().collect());
        let mut inb: HashMap<u32, [Defs; 6]> = HashMap::new();
        inb.insert(f.entry, entry_st);
        let mut work = vec![f.entry];
        let mut reach_at: HashMap<u32, [Defs; 6]> = HashMap::new();
        while let Some(b) = work.pop() {
            let mut st = inb[&b].clone();
            let mut va = b;
            loop {
                reach_at.insert(va, st.clone());
                let d = defs[&va];
                for k in 0..6 {
                    if d & (1 << k) != 0 {
                        st[k] = [va as i64].into_iter().collect();
                    }
                }
                let i = &f.insns[&va];
                let succ = &f.succ[&va];
                let nxt = va.wrapping_add(i.size);
                if succ.len() == 1 && succ[0] == nxt && !f.leaders.contains(&nxt) {
                    va = nxt;
                    continue;
                }
                for &t in succ {
                    let new = match inb.get(&t) {
                        None => st.clone(),
                        Some(old) => std::array::from_fn(|k| old[k].union(&st[k]).copied().collect()),
                    };
                    if inb.get(&t) != Some(&new) {
                        inb.insert(t, new);
                        work.push(t);
                    }
                }
                break;
            }
        }
        let mut fl = Flags { readers: HashMap::new(), live_defs: HashSet::new(), dynamic: HashSet::new() };
        for (va, u) in uses {
            if u == 0 {
                continue;
            }
            let mut src: Defs = BTreeSet::new();
            let mut per: Vec<&Defs> = Vec::new();
            for k in 0..6 {
                if u & (1 << k) != 0 {
                    per.push(&reach_at[&va][k]);
                    src.extend(reach_at[&va][k].iter().copied());
                }
            }
            if src.iter().any(|&x| x == ENTRY || f.calls.contains_key(&(x as u32))) {
                return Err(Refused(format!("flags read at {:08x} come from a call or the entry", va)));
            }
            if per.iter().any(|p| *p != per[0]) {
                return Err(Refused(format!("flags read at {:08x} come from different instructions", va)));
            }
            let mut kinds: Vec<Option<(u8, u8)>> = Vec::new();
            for &x in &src {
                let k = kind_of(f, x as u32);
                if !kinds.contains(&k) {
                    kinds.push(k);
                }
            }
            if kinds.len() > 1 {
                fl.dynamic.extend(src.iter().map(|&x| x as u32));
            }
            let kd = if kinds.len() == 1 { kinds[0] } else { None };
            fl.readers.insert(va, (src.iter().copied().collect(), kd));
            fl.live_defs.extend(src.iter().map(|&x| x as u32));
        }
        let mut ld: Vec<u32> = fl.live_defs.iter().copied().collect();
        ld.sort();
        for d in ld {
            if kind_of(f, d).is_none() {
                return Err(Refused(format!("flags of {} read at {:08x}", f.insns[&d].mnemonic, d)));
            }
        }
        fl.check_slots(f)?;
        Ok(fl)
    }

    /// The shared operand slots: no other storing definer between a definer
    /// and its reader.
    fn check_slots(&self, f: &Func) -> Result<(), Refused> {
        let mut inb: HashMap<u32, Defs> = HashMap::new();
        inb.insert(f.entry, [ENTRY].into_iter().collect());
        let mut work = vec![f.entry];
        let mut last: HashMap<u32, Defs> = HashMap::new();
        while let Some(b) = work.pop() {
            let mut st = inb[&b].clone();
            let mut va = b;
            loop {
                last.insert(va, st.clone());
                if self.live_defs.contains(&va) {
                    st = [va as i64].into_iter().collect();
                }
                let i = &f.insns[&va];
                let succ = &f.succ[&va];
                let nxt = va.wrapping_add(i.size);
                if succ.len() == 1 && succ[0] == nxt && !f.leaders.contains(&nxt) {
                    va = nxt;
                    continue;
                }
                for &t in succ {
                    let new = match inb.get(&t) {
                        None => st.clone(),
                        Some(old) => old.union(&st).copied().collect(),
                    };
                    if inb.get(&t) != Some(&new) {
                        inb.insert(t, new);
                        work.push(t);
                    }
                }
                break;
            }
        }
        // In the order recompile.py's readers dict has them: the insns' order.
        for va in f.insns.keys() {
            if let Some((src, _)) = self.readers.get(va) {
                let s: Defs = src.iter().copied().collect();
                if last[va] != s {
                    return Err(Refused(format!("flags at {:08x}: another instruction's operands are in the way", va)));
                }
            }
        }
        Ok(())
    }
}
