//! Game.exe as mapped (pefile's get_memory_mapped_image), its imports and
//! what each one's ret pops, and its instructions (recompile.py's Image).

use crate::cs::{Cs, Insn};
use crate::omap::OMap;
use crate::Refused;
use std::cell::RefCell;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::rc::Rc;

pub struct Image {
    pub base: u32,
    pub mem: Vec<u8>,
    pub text_lo: u32,
    pub text_hi: u32,
    /// IAT slot -> (dll, name or #ordinal), in pefile's order.
    pub imports: OMap<u32, (String, String)>,
    pub import_pops: HashMap<u32, Option<i64>>,
    cs: Cs,
    cache: RefCell<HashMap<u32, Rc<Insn>>>,
}

fn rd16(d: &[u8], o: usize) -> u32 {
    if o + 2 > d.len() {
        return 0;
    }
    u16::from_le_bytes([d[o], d[o + 1]]) as u32
}

fn rd32(d: &[u8], o: usize) -> u32 {
    if o + 4 > d.len() {
        return 0;
    }
    u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}

fn cstr(d: &[u8], o: usize) -> Vec<u8> {
    let mut out = Vec::new();
    let mut k = o;
    while k < d.len() && d[k] != 0 {
        out.push(d[k]);
        k += 1;
    }
    out
}

struct Section {
    name: Vec<u8>,
    vsize: u32,
    va: u32,
    raw_size: u32,
    raw_ptr: u32,
}

/// pefile's cache_adjust_SectionAlignment.
fn adjust_section_alignment(val: u32, section_alignment: u32, file_alignment: u32) -> u32 {
    let sa = if section_alignment < 0x1000 { file_alignment } else { section_alignment };
    if sa != 0 && val % sa != 0 {
        return sa * (val / sa);
    }
    val
}

impl Image {
    pub fn load(path: &Path, src_root: &Path) -> Result<Image, String> {
        let data = std::fs::read(path).map_err(|e| format!("{}: {}", path.display(), e))?;
        let pe = rd32(&data, 0x3c) as usize;
        if data.get(pe..pe + 4) != Some(b"PE\0\0") {
            return Err(format!("{}: not a PE file", path.display()));
        }
        let coff = pe + 4;
        let nsec = rd16(&data, coff + 2) as usize;
        let opt_size = rd16(&data, coff + 16) as usize;
        let opt = coff + 20;
        let base = rd32(&data, opt + 28);
        let section_alignment = rd32(&data, opt + 32);
        let file_alignment = rd32(&data, opt + 36);
        let import_rva = rd32(&data, opt + 96 + 8);
        let sec0 = opt + opt_size;
        let mut sections = Vec::new();
        for k in 0..nsec {
            let s = sec0 + 40 * k;
            sections.push(Section {
                name: data[s..s + 8].to_vec(),
                vsize: rd32(&data, s + 8),
                va: rd32(&data, s + 12),
                raw_size: rd32(&data, s + 16),
                raw_ptr: rd32(&data, s + 20),
            });
        }
        // The header: up to the lowest section's raw data, or the end of the
        // section table if that comes later.
        let table_end = sec0 + 40 * nsec;
        let lowest = sections.iter().filter(|s| s.raw_ptr > 0).map(|s| (s.raw_ptr & !0x1ff) as usize).min();
        let hdr = match lowest {
            Some(l) if l != 0 && l >= table_end => l,
            _ => table_end,
        };
        let mut mem: Vec<u8> = data[..hdr.min(data.len())].to_vec();
        for s in &sections {
            if s.vsize == 0 && s.raw_size == 0 {
                continue;
            }
            let srd = s.raw_size as usize;
            let prd = (s.raw_ptr & !0x1ff) as usize;
            let va = adjust_section_alignment(s.va, section_alignment, file_alignment) as usize;
            if srd > data.len() || prd > data.len() || srd + prd > data.len() || va >= 0x1000_0000 {
                continue;
            }
            if va > mem.len() {
                mem.resize(va, 0);
            } else {
                mem.truncate(va);
            }
            let end = (prd + srd).min(s.raw_ptr as usize + srd).min(data.len());
            mem.extend_from_slice(&data[prd..end.max(prd)]);
        }
        let text = sections
            .iter()
            .find(|s| {
                let n: Vec<u8> = s.name.iter().copied().take_while(|&b| b != 0).collect();
                n == b".text"
            })
            .ok_or("no .text")?;
        let text_lo = base + text.va;
        let text_hi = text_lo + text.vsize;
        // The imports, from the mapped image as pefile reads them.
        let mut imports = OMap::new();
        if import_rva != 0 {
            let mut d = import_rva as usize;
            loop {
                let oft = rd32(&mem, d);
                let name_rva = rd32(&mem, d + 12);
                let ft = rd32(&mem, d + 16);
                if oft == 0 && name_rva == 0 && ft == 0 && rd32(&mem, d + 4) == 0 && rd32(&mem, d + 8) == 0 {
                    break;
                }
                let dll = String::from_utf8_lossy(&cstr(&mem, name_rva as usize)).into_owned();
                let ilt = if oft != 0 { oft } else { ft };
                let mut k = 0usize;
                loop {
                    let v = rd32(&mem, ilt as usize + 4 * k);
                    if v == 0 {
                        break;
                    }
                    let name = if v & 0x8000_0000 != 0 {
                        // pefile names some DLLs' ordinals from its own tables (ord<n> where it has none).
                        let ord = v & 0xffff;
                        match crate::ordlookup::table(&dll.to_lowercase()) {
                            Some(t) => t.iter().find(|(o, _)| *o == ord).map(|(_, n)| n.to_string()).unwrap_or(format!("ord{}", ord)),
                            None => format!("#{}", ord),
                        }
                    } else {
                        String::from_utf8_lossy(&cstr(&mem, v as usize + 2)).into_owned()
                    };
                    imports.insert(base + ft + 4 * k as u32, (dll.clone(), name));
                    k += 1;
                }
                d += 20;
            }
        }
        let import_pops = import_pops(&imports, src_root);
        Ok(Image {
            base,
            mem,
            text_lo,
            text_hi,
            imports,
            import_pops,
            cs: Cs::new(),
            cache: RefCell::new(HashMap::new()),
        })
    }

    pub fn u32(&self, va: u32) -> u32 {
        let o = va.wrapping_sub(self.base) as usize;
        let mut v = 0u32;
        for k in 0..4 {
            if let Some(&b) = self.mem.get(o + k) {
                v |= (b as u32) << (8 * k);
            }
        }
        v
    }

    pub fn u8(&self, va: u32) -> u8 {
        self.mem[va.wrapping_sub(self.base) as usize]
    }

    pub fn in_text(&self, va: u32) -> bool {
        self.text_lo <= va && va < self.text_hi
    }

    /// The instruction at va; `rep ret` (F3 C3) is a ret (issue #135).
    pub fn insn(&self, va: u32) -> Result<Rc<Insn>, Refused> {
        if let Some(i) = self.cache.borrow().get(&va) {
            return Ok(i.clone());
        }
        if !self.in_text(va) {
            return Err(Refused(format!("code outside .text at {:08x}", va)));
        }
        let o = (va - self.base) as usize;
        let lo = o.min(self.mem.len());
        let hi = (o + 16).min(self.mem.len());
        let mut i = match self.cs.disasm1(&self.mem[lo..hi], va) {
            Some(i) => i,
            None => return Err(Refused(format!("no instruction decodes at {:08x}", va))),
        };
        if i.mnemonic == "repz ret" || i.mnemonic == "rep ret" {
            i.mnemonic = "ret".to_string();
        }
        let i = Rc::new(i);
        self.cache.borrow_mut().insert(va, i.clone());
        Ok(i)
    }
}

fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

fn is_space(c: char) -> bool {
    c.is_whitespace() || ('\u{1c}'..='\u{1f}').contains(&c)
}

/// recompile.py's `\b[A-Z][A-Za-z0-9_]*\(\s*"([A-Za-z0-9_]+)",\s*(\d+)\s*,`
/// over a shim source, each (name, argc) in order.
fn shim_decls(text: &str, out: &mut Vec<(String, i64)>) {
    let s: Vec<char> = text.chars().collect();
    let n = s.len();
    let ident = |c: char| c.is_ascii_alphanumeric() || c == '_';
    let mut p = 0;
    while p < n {
        let m = (|| -> Option<(usize, String, i64)> {
            if !s[p].is_ascii_uppercase() || (p > 0 && is_word(s[p - 1])) {
                return None;
            }
            let mut k = p + 1;
            while k < n && ident(s[k]) {
                k += 1;
            }
            if k >= n || s[k] != '(' {
                return None;
            }
            k += 1;
            while k < n && is_space(s[k]) {
                k += 1;
            }
            if k >= n || s[k] != '"' {
                return None;
            }
            k += 1;
            let a = k;
            while k < n && ident(s[k]) {
                k += 1;
            }
            if k == a || k >= n || s[k] != '"' {
                return None;
            }
            let name: String = s[a..k].iter().collect();
            k += 1;
            if k >= n || s[k] != ',' {
                return None;
            }
            k += 1;
            while k < n && is_space(s[k]) {
                k += 1;
            }
            let d = k;
            while k < n && s[k].is_ascii_digit() {
                k += 1;
            }
            if k == d {
                return None;
            }
            let num: String = s[d..k].iter().collect();
            while k < n && is_space(s[k]) {
                k += 1;
            }
            if k >= n || s[k] != ',' {
                return None;
            }
            Some((k + 1, name, num.parse().ok()?))
        })();
        match m {
            Some((end, name, argc)) => {
                out.push((name, argc));
                p = end;
            }
            None => p += 1,
        }
    }
}

fn glob_dir(dir: &Path, prefix: &str, suffix: &str) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Ok(rd) = std::fs::read_dir(dir) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if !n.starts_with('.') && n.starts_with(prefix) && n.ends_with(suffix) && n.len() >= prefix.len() + suffix.len()
            {
                out.push(dir.join(n));
            }
        }
    }
    out
}

/// What each import's ret pops (recompile.py's import_pops): from our
/// runtime's shims, which declare their argument counts, and from decorated
/// names; wsprintfA is cdecl.
fn import_pops(imports: &OMap<u32, (String, String)>, root: &Path) -> HashMap<u32, Option<i64>> {
    let mut files = glob_dir(&root.join("src/runtime"), "rt_shims_", ".cpp");
    files.extend(glob_dir(&root.join("third_party/winx86/src/runtime"), "", ".cpp"));
    files.sort_by(|a, b| a.to_string_lossy().cmp(&b.to_string_lossy()));
    let mut argc: HashMap<String, i64> = HashMap::new();
    for f in files {
        if let Ok(b) = std::fs::read(&f) {
            let mut decls = Vec::new();
            shim_decls(&String::from_utf8_lossy(&b), &mut decls);
            for (name, n) in decls {
                argc.entry(name).or_insert(n);
            }
        }
    }
    let mut out = HashMap::new();
    for (slot, (_, name)) in imports.iter() {
        let v = if name == "wsprintfA" || name == "wsprintfW" {
            Some(0)
        } else if let Some(at) = name.rfind('@').filter(|&k| {
            let t = &name[k + 1..];
            !t.is_empty() && t.chars().all(|c| c.is_ascii_digit())
        }) {
            name[at + 1..].parse().ok()
        } else if let Some(n) = argc.get(name) {
            Some(4 * n)
        } else if name == "timeGetTime" {
            Some(0)
        } else {
            None
        };
        out.insert(*slot, v);
    }
    out
}
