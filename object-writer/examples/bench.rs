//! Instrumented writer benchmark: prebuilt inputs, serialization only.
use asterasm_object::*;
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering::Relaxed};
use std::time::Instant;
struct Counting;
static ALLOCS: AtomicUsize = AtomicUsize::new(0);
static LIVE: AtomicUsize = AtomicUsize::new(0);
static PEAK: AtomicUsize = AtomicUsize::new(0);
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let p = unsafe { System.alloc(layout) };
        if !p.is_null() {
            ALLOCS.fetch_add(1, Relaxed);
            let live = LIVE.fetch_add(layout.size(), Relaxed) + layout.size();
            PEAK.fetch_max(live, Relaxed);
        }
        p
    }
    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        LIVE.fetch_sub(layout.size(), Relaxed);
        unsafe { System.dealloc(p, layout) };
    }
}
#[global_allocator]
static ALLOCATOR: Counting = Counting;
fn bytes(data: &[u8]) -> Bytes {
    Bytes {
        data: data.as_ptr(),
        size: data.len() as u64,
    }
}
fn bench(data_size: usize, symbols_count: usize, label: &str) {
    let data = vec![0u8; data_size];
    let names: Vec<_> = (0..symbols_count)
        .map(|n| format!("function_{n}"))
        .collect();
    let symbols: Vec<_> = names
        .iter()
        .enumerate()
        .map(|(n, name)| InputSymbol {
            name: bytes(name.as_bytes()),
            value: (n * 16) as u64,
            size: 16,
            section: 0,
            binding: 1,
            kind: 1,
            reserved: 0,
        })
        .collect();
    let relocations: Vec<_> = (0..symbols_count)
        .map(|n| Relocation {
            offset: (n * 16) as u64,
            addend: 0,
            section: 0,
            symbol: ((n + 1) % symbols_count) as u32,
            kind: 1,
            reserved: 0,
        })
        .collect();
    let section = Section {
        name: bytes(b".text"),
        data: bytes(&data),
        size: data.len() as u64,
        alignment: 16,
        kind: 1,
        reserved: 0,
    };
    for format in 1..=4 {
        let m = Module {
            abi_version: 1,
            format,
            architecture: 1,
            reserved: 0,
            sections: &section,
            section_count: 1,
            symbols: symbols.as_ptr(),
            symbol_count: symbols.len() as u64,
            relocations: relocations.as_ptr(),
            relocation_count: relocations.len() as u64,
            base_address: 0x100000,
            image_limit: data.len() as u64,
            entry_symbol: 0,
            riscv_float_abi: 0,
        };
        let mut output = Output::default();
        unsafe {
            assert_eq!(ast_object_write(&m, &mut output), 0);
            ast_object_output_free(&mut output);
        }
        let baseline = LIVE.load(Relaxed);
        PEAK.store(baseline, Relaxed);
        ALLOCS.store(0, Relaxed);
        let start = Instant::now();
        let mut output_bytes = 0;
        for _ in 0..20 {
            unsafe {
                assert_eq!(ast_object_write(&m, &mut output), 0);
            }
            output_bytes = output.size;
            unsafe {
                ast_object_output_free(&mut output);
            }
        }
        let seconds = start.elapsed().as_secs_f64();
        let allocations = ALLOCS.load(Relaxed) / 20;
        let peak = PEAK.load(Relaxed) - baseline;
        println!(
            "{label},format={format},input_bytes={data_size},output_bytes={output_bytes},symbols={symbols_count},ms={:.3},input_MiB_s={:.1},allocations={allocations},peak_extra_bytes={peak}",
            seconds * 1000.0 / 20.0,
            20.0 * data_size as f64 / seconds / 1048576.0
        );
    }
}
fn main() {
    bench(20000 * 64, 20000, "many-functions");
    bench(8 * 1024 * 1024, 100, "large-data");
}
