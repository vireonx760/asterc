use super::relocations::word;
use super::*;

fn aligned(value: u64, alignment: u64) -> Result<u64> {
    value
        .checked_add(alignment - 1)
        .map(|v| v & !(alignment - 1))
        .ok_or_else(|| range("Flat layout overflow"))
}
fn put_word(data: &mut [u8], value: u32) {
    data[..4].copy_from_slice(&value.to_le_bytes());
}
fn signed(value: i128, bits: u32) -> Result<i64> {
    if value < -(1i128 << (bits - 1)) || value >= (1i128 << (bits - 1)) {
        return Err(range("Flat relocation exceeds its signed range"));
    }
    Ok(value as i64)
}
fn patch(data: &mut [u8], kind: u32, pc: u64, target: u64) -> Result<()> {
    let delta = target as i128 - pc as i128;
    match kind {
        0 | 7 => put_word(data, signed(delta - 4, 32)? as u32),
        1 => data[..8].copy_from_slice(&target.to_le_bytes()),
        8 => {
            let value = u32::try_from(target)
                .map_err(|_| range("Flat ABS32 relocation exceeds 32 bits"))?;
            put_word(data, value);
        }
        2 => {
            if pc & 3 != 0 || target & 3 != 0 {
                return Err(range("Unaligned A64 call"));
            }
            let imm = signed(delta / 4, 26)? as u32 & 0x03ffffff;
            put_word(data, (word(data) & 0xfc000000) | imm);
        }
        3 | 5 => {
            if pc & 3 != 0 {
                return Err(range("Unaligned A64 address instruction"));
            }
            let offset = if kind == 5 {
                (target >> 12) as i128 - (pc >> 12) as i128
            } else {
                delta
            };
            let imm = signed(offset, 21)? as u32 & 0x1fffff;
            put_word(
                data,
                (word(data) & 0x9f00001f) | ((imm & 3) << 29) | ((imm >> 2) << 5),
            );
            if kind == 5 {
                let lo = word(&data[4..]);
                put_word(
                    &mut data[4..],
                    (lo & !0x003ffc00) | ((target as u32 & 4095) << 10),
                );
            }
        }
        4 | 6 => {
            if pc & 3 != 0 || (kind == 4 && target & 1 != 0) {
                return Err(range("Unaligned RISC-V call/address pair"));
            }
            let d = signed(delta, 32)?;
            let low = (d << 52) >> 52;
            let high = signed(((d - low) / 4096) as i128, 20)?;
            let hi = word(data);
            let lo = word(&data[4..]);
            put_word(data, (hi & 4095) | ((high as u32 & 0xfffff) << 12));
            put_word(&mut data[4..], (lo & 0xfffff) | ((low as u32 & 4095) << 20));
        }
        _ => return Err(unsupported("Unknown flat relocation")),
    }
    Ok(())
}
pub(super) fn write(c: &Checked<'_>) -> Result<(Vec<u8>, u64)> {
    let m = c.module;
    let mut cursor = m.base_address;
    let mut addresses = Vec::with_capacity(c.sections.len());
    for s in c.sections {
        cursor = aligned(cursor, s.alignment)?;
        addresses.push(cursor);
        cursor = cursor
            .checked_add(s.size)
            .ok_or_else(|| range("Flat section exceeds address space"))?;
    }
    let length = cursor - m.base_address;
    if length > m.image_limit || length > isize::MAX as u64 {
        return Err(range("Flat image exceeds the caller's image_limit"));
    }
    let mut bytes = Vec::with_capacity(length as usize);
    for (i, data) in c.data.iter().enumerate() {
        let offset = (addresses[i] - m.base_address) as usize;
        bytes.resize(offset, 0); // Only padding/BSS need zero initialization.
        bytes.extend_from_slice(data);
        bytes.resize(offset + c.sections[i].size as usize, 0);
    }
    let symbol_address = |id: u32| -> Result<u64> {
        let s = &c.symbols[id as usize];
        match s.section {
            UNDEFINED => Err(Error(
                4,
                format!(
                    "Unresolved flat symbol {}",
                    String::from_utf8_lossy(c.symbol_names[id as usize])
                ),
            )),
            ABSOLUTE => Ok(s.value),
            _ => addresses[s.section as usize]
                .checked_add(s.value)
                .ok_or_else(|| range("Flat symbol overflow")),
        }
    };
    for r in c.relocs {
        let target = symbol_address(r.symbol)?
            .checked_add_signed(r.addend)
            .ok_or_else(|| range("Flat symbol/addend overflow"))?;
        let pc = addresses[r.section as usize] + r.offset;
        patch(
            &mut bytes[(pc - m.base_address) as usize..],
            r.kind,
            pc,
            target,
        )?;
    }
    let entry = if m.entry_symbol == UNDEFINED {
        0
    } else {
        symbol_address(m.entry_symbol)?
    };
    Ok((bytes, entry))
}
