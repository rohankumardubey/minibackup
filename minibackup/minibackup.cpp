#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <thread>   // std::thread, std::this_thread::get_id, hardware_concurrency
#include <mutex>    // std::mutex, std::lock_guard
#include <atomic>   // std::atomic
#include <chrono>   // time_since_epoch() used in tmp filename


#include <openssl/sha.h>
#include <zstd.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;
namespace fs = std::filesystem;

#ifndef _WIN32
  #include <unistd.h>   // gethostname
  #include <sys/stat.h>
  #ifdef __APPLE__
    #include <utime.h>
  #else
    #include <fcntl.h>  // AT_FDCWD
    #include <time.h>   // timespec
  #endif
#endif


static constexpr size_t kChunkSize = 1u << 20; // 1 MiB
static const std::string kChunksDir = "chunks";
static const std::string kManifestsDir = "manifests";
static const std::string kVersion = "0.2-zstd";

struct ChunkRef { std::string hash; int size=0; long long offset=0; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChunkRef, hash, size, offset)

struct FileEntry {
  std::string path; long long size=0; uint32_t mode=0; std::time_t mod_time=0;
  std::vector<ChunkRef> chunks;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileEntry, path, size, mode, mod_time, chunks)

struct Snapshot {
  std::string id, version, source_dir, host_name;
  std::time_t started=0, finished=0;
  std::vector<FileEntry> files;
  int total_files=0, stored_chunks=0, reused_chunks=0;
  long long total_bytes=0;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Snapshot, id, version, source_dir, host_name,
  started, finished, files, total_files, total_bytes, stored_chunks, reused_chunks)

static void ensure_dir(const fs::path& p){ std::error_code ec; fs::create_directories(p, ec); if(ec) throw std::runtime_error("mkdir: "+p.string()+": "+ec.message()); }
static bool file_exists(const fs::path& p){ std::error_code ec; return fs::exists(p, ec); }

static std::string hex_encode(const unsigned char* data, size_t len){
  static const char* hex="0123456789abcdef";
  std::string out; out.resize(len*2);
  for(size_t i=0;i<len;++i){ out[2*i]=hex[(data[i]>>4)&0xF]; out[2*i+1]=hex[data[i]&0xF]; }
  return out;
}
static std::string sha256_hex(const unsigned char* data, size_t len){
  unsigned char h[SHA256_DIGEST_LENGTH]; SHA256(data, len, h); return hex_encode(h, SHA256_DIGEST_LENGTH);
}

static fs::path chunk_path(const fs::path& repo, const std::string& hashHex){
  std::string sub = hashHex.substr(0,2);
  return repo / kChunksDir / sub / (hashHex + ".zst");
}
static fs::path manifest_path(const fs::path& repo, const std::string& snapId){
  return repo / kManifestsDir / (snapId + ".json");
}
static std::string now_id(){
  std::time_t t = std::time(nullptr); std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32]; std::strftime(buf,sizeof(buf),"%Y%m%d-%H%M%S",&tm); return buf;
}

static void atomic_write(const fs::path& dst, const std::vector<unsigned char>& data) {
  ensure_dir(dst.parent_path());
  // Unique temp name to avoid collisions under parallelism
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
#ifndef _WIN32
  int pid = static_cast<int>(getpid());
#else
  int pid = 0;
#endif
  fs::path tmp = dst;
  tmp += ".tmp." + std::to_string(pid) + "." + std::to_string(tid) + "." + std::to_string(now);

  std::ofstream out(tmp, std::ios::binary);
  if (!out) throw std::runtime_error("create tmp failed: " + tmp.string());
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  out.close();

  std::error_code ec;
  fs::rename(tmp, dst, ec); // If another thread already created dst, this atomically overwrites — ok for identical content.
  if (ec) throw std::runtime_error("rename failed: " + ec.message());
}


// Zstd helpers
static std::vector<unsigned char> zstd_compress(const unsigned char* data, size_t len, int level=1){
  size_t bound = ZSTD_compressBound(len);
  std::vector<unsigned char> out(bound);
  size_t csz = ZSTD_compress(out.data(), bound, data, len, level);
  if(ZSTD_isError(csz)) throw std::runtime_error(std::string("ZSTD_compress: ")+ZSTD_getErrorName(csz));
  out.resize(csz); return out;
}
static std::vector<unsigned char> zstd_decompress_file_exact(const fs::path& file, size_t expected){
  std::ifstream in(file, std::ios::binary);
  if(!in) throw std::runtime_error("open chunk failed: "+file.string());
  std::vector<unsigned char> comp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<unsigned char> out(expected);
  size_t got = ZSTD_decompress(out.data(), expected, comp.data(), comp.size());
  if(ZSTD_isError(got)) throw std::runtime_error(std::string("ZSTD_decompress: ")+ZSTD_getErrorName(got));
  if(got < expected) out.resize(got);
  return out;
}

struct ProcessResult { FileEntry entry; int stored=0, reused=0; };

static ProcessResult process_file(const fs::path& full, const fs::path& rel, const fs::path& repo){
  ProcessResult res{};
  std::ifstream in(full, std::ios::binary); if(!in) throw std::runtime_error("open: "+full.string());
  auto s = fs::file_size(full);
#ifndef _WIN32
  uint32_t mode = static_cast<uint32_t>(fs::status(full).permissions());
#else
  uint32_t mode = 0644;
#endif
  auto ftime = fs::last_write_time(full);
  auto sys_now = std::chrono::system_clock::now();
  auto file_now = decltype(ftime)::clock::now();
  auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - file_now + sys_now);
  std::time_t mtime = std::chrono::system_clock::to_time_t(sctp);

  res.entry.path = rel.generic_string();
  res.entry.size = static_cast<long long>(s);
  res.entry.mode = mode;
  res.entry.mod_time = mtime;

  std::vector<unsigned char> buf(kChunkSize);
  long long offset=0;
  while(in){
    in.read(reinterpret_cast<char*>(buf.data()), kChunkSize);
    std::streamsize got = in.gcount();
    if(got<=0) break;

    std::string h = sha256_hex(buf.data(), static_cast<size_t>(got));
    fs::path dst = chunk_path(repo, h);
    if(!file_exists(dst)){
      auto z = zstd_compress(buf.data(), static_cast<size_t>(got), 1);
      atomic_write(dst, z); res.stored++;
    }else res.reused++;

    res.entry.chunks.push_back(ChunkRef{h, static_cast<int>(got), offset});
    offset += got;
    if(static_cast<size_t>(got) < kChunkSize) break;
  }
  return res;
}

static Snapshot do_backup(const fs::path& src, const fs::path& repo) {
  Snapshot snap{};
  snap.id = now_id(); snap.version = kVersion;
  snap.source_dir = fs::weakly_canonical(src).string();
  char host[256]={0};
#ifndef _WIN32
  if(gethostname(host, sizeof(host))!=0) std::strncpy(host,"unknown",sizeof(host)-1);
#else
  std::strncpy(host,"windows",sizeof(host)-1);
#endif
  snap.host_name = host; snap.started = std::time(nullptr);

  ensure_dir(repo / kChunksDir);
  ensure_dir(repo / kManifestsDir);

  // Collect files first
  std::vector<std::pair<fs::path, fs::path>> files;
  for (auto const& de : fs::recursive_directory_iterator(src)) {
    if (de.is_regular_file()) {
      fs::path full = de.path();
      fs::path rel  = fs::relative(full, src);
      files.emplace_back(std::move(full), std::move(rel));
    }
  }

  // Parallel workers
  unsigned workers = std::max(1u, std::thread::hardware_concurrency());
  std::atomic<size_t> next{0};
  std::mutex agg_mu;

  auto worker = [&]() {
    while (true) {
      size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= files.size()) break;
      const auto& full = files[i].first;
      const auto& rel  = files[i].second;
      try {
        auto pr = process_file(full, rel, repo);
        std::lock_guard<std::mutex> lk(agg_mu);
        snap.total_files++;
        snap.total_bytes += pr.entry.size;
        snap.stored_chunks += pr.stored;
        snap.reused_chunks += pr.reused;
        snap.files.push_back(std::move(pr.entry));
      } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(agg_mu);
        std::cerr << "Error: " << rel << ": " << e.what() << "\n";
      }
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(workers);
  for (unsigned t = 0; t < workers; ++t) pool.emplace_back(worker);
  for (auto& th : pool) th.join();

  snap.finished = std::time(nullptr);

  // Write manifest
  fs::path mp = manifest_path(repo, snap.id);
  ensure_dir(mp.parent_path());
  std::ofstream out(mp);
  out << std::setw(2) << json(snap) << "\n";
  out.close();

  std::cout << "Snapshot " << snap.id << " created. Files: " << snap.total_files
            << ", Bytes: " << snap.total_bytes
            << ", Chunks: stored=" << snap.stored_chunks
            << " reused=" << snap.reused_chunks << "\n";
  return snap;
}


static Snapshot load_snapshot(const fs::path& p){
  std::ifstream in(p); if(!in) throw std::runtime_error("open manifest: "+p.string());
  json j; in >> j; return j.get<Snapshot>();
}

static void list_snapshots(const fs::path& repo){
  fs::path dir = repo / kManifestsDir;
  if(!file_exists(dir)){ std::cout << "No snapshots found.\n"; return; }
  bool any=false;
  for(auto& e : fs::directory_iterator(dir)){
    if(e.is_regular_file() && e.path().extension()==".json"){
      any=true; try{
        Snapshot s = load_snapshot(e.path());
        std::cout << s.id << " | started=" << std::asctime(std::gmtime(&s.started))
                  << "  files=" << s.total_files
                  << "  bytes=" << s.total_bytes
                  << "  chunks stored=" << s.stored_chunks
                  << " reused=" << s.reused_chunks << "\n";
      }catch(...){ std::cout << e.path().filename().string() << " (failed)\n"; }
    }
  }
  if(!any) std::cout << "No snapshots found.\n";
}

static void do_restore(const fs::path& repo, const std::string& id, const fs::path& dst){
  Snapshot s = load_snapshot(manifest_path(repo, id));
  for(const auto& fe : s.files){
    fs::path outPath = dst / fs::path(fe.path);
    ensure_dir(outPath.parent_path());
    fs::path tmp = outPath; tmp += ".tmp";
    std::ofstream out(tmp, std::ios::binary);
    if(!out) throw std::runtime_error("create out: "+tmp.string());
#ifndef _WIN32
    ::chmod(tmp.c_str(), static_cast<mode_t>(fe.mode & 0777));
#endif
    for(const auto& cr : fe.chunks){
      fs::path cp = chunk_path(repo, cr.hash);
      auto data = zstd_decompress_file_exact(cp, static_cast<size_t>(cr.size));
      out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    out.close();
#ifndef _WIN32
  #ifdef __APPLE__
    struct utimbuf ub; ub.actime=std::time(nullptr); ub.modtime=fe.mod_time; ::utime(tmp.c_str(), &ub);
  #else
    struct timespec ts[2]; ts[0].tv_sec=std::time(nullptr); ts[0].tv_nsec=0; ts[1].tv_sec=fe.mod_time; ts[1].tv_nsec=0;
    ::utimensat(AT_FDCWD, tmp.c_str(), ts, 0);
  #endif
#endif
    std::error_code ec; fs::rename(tmp, outPath, ec); if(ec) throw std::runtime_error("rename: "+ec.message());
  }
  std::cout << "Restored snapshot " << id << " to " << fs::weakly_canonical(dst) << "\n";
}

static void do_verify(const fs::path& repo, const std::string& id){
  Snapshot s = load_snapshot(manifest_path(repo, id));
  std::vector<std::string> missing;
  for(const auto& fe : s.files) for(const auto& cr : fe.chunks){
    fs::path p = chunk_path(repo, cr.hash); if(!file_exists(p)) missing.push_back(cr.hash);
  }
  if(missing.empty()) std::cout << "Snapshot " << id << ": all chunks present.\n";
  else{
    std::cout << "Snapshot " << id << ": " << missing.size() << " missing chunks.\n";
    for(auto& h:missing) std::cout << "  - " << h << "\n";
    throw std::runtime_error("integrity check failed");
  }
}

static void req(bool c, const std::string& m){ if(!c){ std::cerr<<m<<"\n"; std::exit(2);} }

int main(int argc, char** argv){
  if(argc<2){ std::cout<<"minibackup "<<kVersion<<"\nCommands: backup | list | restore | verify\n"; return 1; }
  std::string cmd=argv[1];
  auto get=[&](const char* name)->std::optional<std::string>{
    for(int i=2;i+1<argc;++i) if(std::string(argv[i])==name) return std::string(argv[i+1]); return std::nullopt;
  };
  try{
    if(cmd=="backup"){
        auto src=get("--src"), repo=get("--repo");
        req(src.has_value() && repo.has_value(), "backup requires --src and --repo");
        do_backup(*src, *repo);
    }else if(cmd=="list"){
        auto repo=get("--repo");
        req(repo.has_value(), "list requires --repo");
        list_snapshots(*repo);
    }else if(cmd=="restore"){
        auto repo=get("--repo"), id=get("--snapshot"), dst=get("--dst");
        req(repo.has_value() && id.has_value() && dst.has_value(),
            "restore requires --repo --snapshot --dst");
        do_restore(*repo, *id, *dst);
    }else if(cmd=="verify"){
        auto repo=get("--repo"), id=get("--snapshot");
        req(repo.has_value() && id.has_value(), "verify requires --repo --snapshot");
        do_verify(*repo, *id);
    }else{ std::cerr<<"unknown command: "<<cmd<<"\n"; return 2; }
  }catch(const std::exception& e){ std::cerr<<"error: "<<e.what()<<"\n"; return 3; }
  return 0;
}
