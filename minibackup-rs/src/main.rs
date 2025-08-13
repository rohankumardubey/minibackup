use anyhow::{bail, Context, Result};
use chrono::Utc;
use clap::{Parser, Subcommand};
use dashmap::DashSet;
use filetime::{set_file_times, FileTime};
use hostname as hn;
use memmap2::MmapOptions;
use openssl::sha::sha256;
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use walkdir::WalkDir;
use zstd_safe::{decompress as zstd_decompress, CCtx, CParameter, compress_bound};

#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;

const CHUNK_SIZE: usize = 1 << 20; // 1 MiB
const CHUNKS_DIR: &str = "chunks";
const MANIFESTS_DIR: &str = "manifests";
const VERSION: &str = "0.4.1-zstdsafe";

#[derive(Debug, Serialize, Deserialize, Clone)]
struct ChunkRef { hash: String, size: usize, offset: i64 }

#[derive(Debug, Serialize, Deserialize, Clone)]
struct FileEntry {
    path: String, size: i64, mode: u32, mod_time: i64, chunks: Vec<ChunkRef>,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
struct Snapshot {
    id: String, version: String, source_dir: String, host_name: String,
    started: i64, finished: i64, files: Vec<FileEntry>,
    total_files: i32, total_bytes: i64, stored_chunks: i32, reused_chunks: i32,
}

#[derive(Parser)]
#[command(name = "minibackup-rs", version = VERSION)]
struct Cli { #[command(subcommand)] cmd: Commands }

#[derive(Subcommand)]
enum Commands {
    Backup { #[arg(long)] src: PathBuf, #[arg(long)] repo: PathBuf },
    List   { #[arg(long)] repo: PathBuf },
    Restore{ #[arg(long)] repo: PathBuf, #[arg(long)] snapshot: String, #[arg(long)] dst: PathBuf },
    Verify { #[arg(long)] repo: PathBuf, #[arg(long)] snapshot: String },
}

fn ensure_dir(p: &Path) -> Result<()> {
    fs::create_dir_all(p).with_context(|| format!("mkdir -p {}", p.display()))
}
fn chunk_path(repo: &Path, hash_hex: &str) -> PathBuf {
    repo.join(CHUNKS_DIR).join(&hash_hex[0..2]).join(format!("{hash_hex}.zst"))
}
fn manifest_path(repo: &Path, snap_id: &str) -> PathBuf {
    repo.join(MANIFESTS_DIR).join(format!("{snap_id}.json"))
}
fn atomic_write(path: &Path, data: &[u8]) -> Result<()> {
    if let Some(parent) = path.parent() { ensure_dir(parent)?; }
    let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let tmp = path.with_extension(format!("tmp.{}.{}", std::process::id(), nanos));
    { let mut f = File::create(&tmp)?; f.write_all(data)?; f.flush()?; }
    fs::rename(&tmp, path).with_context(|| format!("rename {} -> {}", tmp.display(), path.display()))?;
    Ok(())
}
#[inline] fn sha256_hex(bytes: &[u8]) -> String { hex::encode(sha256(bytes)) }

/// Build a thread-safe set of existing chunk hashes (without ".zst")
fn build_chunk_index(repo: &Path) -> Arc<DashSet<String>> {
    let set = DashSet::new();
    let root = repo.join(CHUNKS_DIR);
    if let Ok(dirs) = fs::read_dir(&root) {
        for dir in dirs.flatten() {
            if dir.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                if let Ok(files) = fs::read_dir(dir.path()) {
                    for f in files.flatten() {
                        if f.file_type().map(|t| t.is_file()).unwrap_or(false) {
                            if let Some(stem) = f.path().file_stem().and_then(|s| s.to_str()) {
                                set.insert(stem.to_owned());
                            }
                        }
                    }
                }
            }
        }
    }
    Arc::new(set)
}

struct ProcessResult { entry: FileEntry, stored: i32, reused: i32 }

fn process_file(full: &Path, rel: &Path, repo: &Path, index: &Arc<DashSet<String>>) -> Result<ProcessResult> {
    let meta = fs::metadata(full)?;
    if !meta.is_file() { bail!("not a file: {}", full.display()); }

    let mode: u32 = {
        #[cfg(unix)] { meta.permissions().mode() }
        #[cfg(not(unix))] { 0o644 }
    };
    let mod_time = meta.modified().ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_secs() as i64).unwrap_or(0);

    let size = meta.len() as i64;
    let mut entry = FileEntry {
        path: rel.to_string_lossy().replace('\\', "/"),
        size, mode, mod_time, chunks: Vec::new(),
    };

    // Reusable zstd context + prealloc out buffer (per file)
    let mut cctx = CCtx::create();
    cctx.set_parameter(CParameter::CompressionLevel(1))
        .map_err(|e| anyhow::anyhow!("zstd set param: {:?}", e))?;
    let mut out_buf = vec![0u8; compress_bound(CHUNK_SIZE)];

    let mut stored = 0;
    let mut reused = 0;

    // Try mmap first
    let try_mmap = unsafe { MmapOptions::new().map(&File::open(full)?) };
    match try_mmap {
        Ok(mmap) => {
            let mut off = 0usize;
            while off < mmap.len() {
                let end = (off + CHUNK_SIZE).min(mmap.len());
                let chunk = &mmap[off..end];
                let h = sha256_hex(chunk);
                let dst = chunk_path(repo, &h);

                // CLAIM FIRST: if we inserted, we own writing; else, skip compression
                if index.insert(h.clone()) {
                    let written = cctx.compress2(out_buf.as_mut_slice(), chunk)
                        .map_err(|e| anyhow::anyhow!("zstd compress2: {:?}", e))?;
                    atomic_write(&dst, &out_buf[..written])?;
                    stored += 1;
                } else {
                    reused += 1;
                }

                entry.chunks.push(ChunkRef { hash: h, size: chunk.len(), offset: off as i64 });
                off = end;
            }
        }
        Err(_) => {
            let mut f = File::open(full)?;
            let mut in_buf = vec![0u8; CHUNK_SIZE];
            let mut offset: i64 = 0;
            loop {
                let n = f.read(&mut in_buf)?;
                if n == 0 { break; }
                let chunk = &in_buf[..n];
                let h = sha256_hex(chunk);
                let dst = chunk_path(repo, &h);

                if index.insert(h.clone()) {
                    let written = cctx.compress2(out_buf.as_mut_slice(), chunk)
                        .map_err(|e| anyhow::anyhow!("zstd compress2: {:?}", e))?;
                    atomic_write(&dst, &out_buf[..written])?;
                    stored += 1;
                } else {
                    reused += 1;
                }

                entry.chunks.push(ChunkRef { hash: h, size: n, offset });
                offset += n as i64;
                if n < CHUNK_SIZE { break; }
            }
        }
    }

    Ok(ProcessResult { entry, stored, reused })
}

fn now_id() -> String { Utc::now().format("%Y%m%d-%H%M%S").to_string() }
fn hostname() -> String {
    hn::get()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|_| "unknown".into())
}

fn do_backup(src: &Path, repo: &Path) -> Result<Snapshot> {
    ensure_dir(&repo.join(CHUNKS_DIR))?;
    ensure_dir(&repo.join(MANIFESTS_DIR))?;

    let mut snap = Snapshot {
        id: now_id(), version: VERSION.into(),
        source_dir: fs::canonicalize(src).unwrap_or(src.to_path_buf()).to_string_lossy().into(),
        host_name: hostname(), started: Utc::now().timestamp(), finished: 0,
        files: Vec::new(), total_files: 0, total_bytes: 0, stored_chunks: 0, reused_chunks: 0,
    };

    // Thread-safe set of existing chunks (preload from repo)
    let index = build_chunk_index(repo);

    // Collect files then process in parallel
    let files: Vec<(PathBuf, PathBuf)> = WalkDir::new(src)
        .into_iter().filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
        .map(|e| (e.path().to_path_buf(), e.path().strip_prefix(src).unwrap().to_path_buf()))
        .collect();

    let results: Vec<ProcessResult> = files.par_iter()
        .filter_map(|(full, rel)| process_file(full, rel, repo, &index).ok())
        .collect();

    for res in results {
        snap.total_files += 1;
        snap.total_bytes += res.entry.size;
        snap.stored_chunks += res.stored;
        snap.reused_chunks += res.reused;
        snap.files.push(res.entry);
    }
    snap.finished = Utc::now().timestamp();

    let mp = manifest_path(repo, &snap.id);
    if let Some(dir) = mp.parent() { ensure_dir(dir)?; }
    let mut f = File::create(&mp)?;
    serde_json::to_writer_pretty(&mut f, &snap)?; writeln!(f)?;
    println!(
        "Snapshot {} created. Files: {}, Bytes: {}, Chunks: stored={} reused={}",
        snap.id, snap.total_files, snap.total_bytes, snap.stored_chunks, snap.reused_chunks
    );
    Ok(snap)
}

fn load_snapshot(path: &Path) -> Result<Snapshot> {
    let f = File::open(path)?; let s: Snapshot = serde_json::from_reader(f)?; Ok(s)
}

fn list_snapshots(repo: &Path) -> Result<()> {
    let dir = repo.join(MANIFESTS_DIR);
    if !dir.exists() { println!("No snapshots found."); return Ok(()); }
    let mut any = false;
    for ent in fs::read_dir(&dir)? {
        let ent = ent?;
        if ent.file_type()?.is_file() && ent.path().extension().map(|e| e == "json").unwrap_or(false) {
            any = true;
            match load_snapshot(&ent.path()) {
                Ok(s) => println!(
                    "{} | started={} | files={} | bytes={} | chunks stored={} reused={}",
                    s.id,
                    chrono::DateTime::<Utc>::from_timestamp(s.started, 0).unwrap_or_else(|| Utc::now()).to_rfc3339(),
                    s.total_files, s.total_bytes, s.stored_chunks, s.reused_chunks
                ),
                Err(_) => println!("{} (failed to load)", ent.file_name().to_string_lossy()),
            }
        }
    }
    if !any { println!("No snapshots found."); }
    Ok(())
}

fn zstd_decompress_file_exact(path: &Path, expected: usize) -> Result<Vec<u8>> {
    let mut f = File::open(path).with_context(|| format!("open chunk {}", path.display()))?;
    let mut comp = Vec::new(); f.read_to_end(&mut comp)?;
    let mut out = vec![0u8; expected];
    let written = zstd_decompress(out.as_mut_slice(), &comp)
        .map_err(|e| anyhow::anyhow!("zstd decompress: {:?}", e))?;
    if written < expected { out.truncate(written); }
    Ok(out)
}

fn do_restore(repo: &Path, id: &str, dst: &Path) -> Result<()> {
    let s = load_snapshot(&manifest_path(repo, id))?;
    for fe in &s.files {
        let out_path = dst.join(&fe.path);
        if let Some(parent) = out_path.parent() { ensure_dir(parent)?; }
        let tmp = out_path.with_extension("tmp");
        {
            let mut out = File::create(&tmp).with_context(|| format!("create {}", tmp.display()))?;
            #[cfg(unix)] {
                let perms = fs::Permissions::from_mode((fe.mode & 0o777) as u32);
                out.set_permissions(perms)?;
            }
            for cr in &fe.chunks {
                let cp = chunk_path(repo, &cr.hash);
                let data = zstd_decompress_file_exact(&cp, cr.size)
                    .with_context(|| format!("decompress {}", cp.display()))?;
                out.write_all(&data)?;
            }
        }
        let atime = FileTime::now();
        let mtime = FileTime::from_unix_time(fe.mod_time, 0);
        set_file_times(&tmp, atime, mtime).ok();
        fs::rename(&tmp, &out_path)
            .with_context(|| format!("rename {} -> {}", tmp.display(), out_path.display()))?;
    }
    println!("Restored snapshot {} to {}", id, dst.display());
    Ok(())
}

fn do_verify(repo: &Path, id: &str) -> Result<()> {
    let s = load_snapshot(&manifest_path(repo, id))?;
    let mut missing = Vec::new();
    for fe in &s.files {
        for cr in &fe.chunks {
            if !chunk_path(repo, &cr.hash).exists() { missing.push(cr.hash.clone()); }
        }
    }
    if missing.is_empty() {
        println!("Snapshot {}: all chunks present.", id);
        Ok(())
    } else {
        println!("Snapshot {}: {} missing chunks.", id, missing.len());
        for h in missing { println!("  - {}", h); }
        bail!("integrity check failed")
    }
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.cmd {
        Commands::Backup { src, repo } => { ensure_dir(&repo)?; do_backup(&src, &repo)?; }
        Commands::List { repo } => { list_snapshots(&repo)?; }
        Commands::Restore { repo, snapshot, dst } => { ensure_dir(&dst)?; do_restore(&repo, &snapshot, &dst)?; }
        Commands::Verify { repo, snapshot } => { do_verify(&repo, &snapshot)?; }
    }
    Ok(())
}
