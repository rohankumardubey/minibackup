// minibackup.c — C11, POSIX (macOS/Linux), parallel file-level backup
// 1 MiB fixed chunks, SHA-256 (OpenSSL), Zstd compression, JSON (Jansson)
// Commands: backup | list | restore | verify

// --- macOS feature macros to expose BSD types (avoid XOPEN hiding them)
#if defined(__APPLE__)
  #ifdef _XOPEN_SOURCE
  #undef _XOPEN_SOURCE
  #endif
  #ifndef _DARWIN_C_SOURCE
  #define _DARWIN_C_SOURCE 1
  #endif
#else
  #define _XOPEN_SOURCE 700
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>     // must precede sys/sysctl.h on macOS
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>    // cpu_count()
#endif

#include <openssl/sha.h>
#include <zstd.h>
#include <jansson.h>

#ifdef __APPLE__
// utime is fine; we’ll use it; utimensat is Linux side
#else
  #include <sys/time.h>
  #include <fcntl.h>
#endif

// ------------ constants
static const size_t CHUNK_SIZE = (1u << 20); // 1 MiB
static const char*  CHUNKS_DIR = "chunks";
static const char*  MANIFESTS_DIR = "manifests";
static const char*  VERSION_STR = "0.4-zstd-c-pthreads-ctxreuse";

// ------------ small utils

static void die(const char* msg) { perror(msg); exit(3); }

static int mkdirs(const char* path) {
  // recursive mkdir -p
  char buf[PATH_MAX];
  size_t len = strnlen(path, sizeof(buf));
  if (len >= sizeof(buf)) return -1;
  memcpy(buf, path, len+1);
  for (char* p = buf + 1; *p; ++p) {
    if (*p == '/') { *p = '\0'; if (mkdir(buf, 0777) && errno != EEXIST) return -1; *p = '/'; }
  }
  if (mkdir(buf, 0777) && errno != EEXIST) return -1;
  return 0;
}

static int ensure_dir_parent(const char* path) {
  char tmp[PATH_MAX];
  strncpy(tmp, path, sizeof(tmp)); tmp[sizeof(tmp)-1] = '\0';
  char* slash = strrchr(tmp, '/');
  if (!slash) return 0;
  *slash = '\0';
  return mkdirs(tmp);
}

static void hex_from_bytes(const unsigned char* in, size_t len, char* out_hex /* 2*len+1 */) {
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out_hex[2*i]   = H[(in[i] >> 4) & 0xF];
    out_hex[2*i+1] = H[in[i] & 0xF];
  }
  out_hex[2*len] = '\0';
}

static void sha256_hex(const void* data, size_t len, char out_hex[65]) {
  unsigned char h[SHA256_DIGEST_LENGTH];
  SHA256((const unsigned char*)data, len, h);
  hex_from_bytes(h, SHA256_DIGEST_LENGTH, out_hex);
}

static int path_join2(char* out, size_t cap, const char* a, const char* b) {
  return snprintf(out, cap, "%s/%s", a, b) >= 0 ? 0 : -1;
}

static void atomic_write_bytes(const char* path, const void* data, size_t len) {
  if (ensure_dir_parent(path)) die("mkdir parent");
  char tmp[PATH_MAX];
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%ld%09ld", path, (int)getpid(), ts.tv_sec, ts.tv_nsec);

  int fd = open(tmp, O_CREAT|O_WRONLY|O_TRUNC, 0666);
  if (fd < 0) die("open tmp");
  ssize_t off = 0;
  const uint8_t* p = (const uint8_t*)data;
  while ((size_t)off < len) {
    ssize_t w = write(fd, p+off, len-off);
    if (w < 0) { close(fd); unlink(tmp); die("write tmp"); }
    off += w;
  }
  fsync(fd);
  close(fd);
  if (rename(tmp, path) != 0) { unlink(tmp); die("rename tmp->final"); }
}

static int file_exists(const char* path) {
  return access(path, F_OK) == 0;
}

// --- portable CPU count
static int cpu_count(void) {
#if defined(__APPLE__)
    int ncpu = 0; size_t len = sizeof(ncpu);
    if (sysctlbyname("hw.logicalcpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0) return ncpu;
    if (sysctlbyname("hw.ncpu",       &ncpu, &len, NULL, 0) == 0 && ncpu > 0) return ncpu;
    return 4;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 4;
#else
    return 4;
#endif
}

// --- chunk subdir cache (avoid mkdirs() every chunk)
static inline int hex2val(char c){ if(c>='0'&&c<='9') return c-'0'; c|=32; if(c>='a'&&c<='f') return 10+(c-'a'); return 0; }
static _Atomic unsigned char g_subdir_ready[256];

static void chunk_path(char out[PATH_MAX], const char* repo, const char hashHex[65]) {
  int idx = (hex2val(hashHex[0])<<4) | hex2val(hashHex[1]);
  if (!atomic_load(&g_subdir_ready[idx])) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s/%c%c", repo, CHUNKS_DIR, hashHex[0], hashHex[1]);
    // mkdirs is idempotent; do once per prefix
    mkdirs(dir);
    atomic_store(&g_subdir_ready[idx], 1);
  }
  snprintf(out, PATH_MAX, "%s/%c%c/%s.zst", repo, hashHex[0], hashHex[1], hashHex);
}

// read whole file into memory (compressed), then decompress into buffer sized to expected_len
static void* zstd_decompress_file_exact(const char* path, size_t expected_len, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if (!f) die("open chunk");
  if (fseek(f, 0, SEEK_END) != 0) die("fseek");
  long sz = ftell(f);
  if (sz < 0) die("ftell");
  rewind(f);
  void* comp = malloc((size_t)sz);
  if (!comp) die("malloc comp");
  if (fread(comp, 1, (size_t)sz, f) != (size_t)sz) die("fread comp");
  fclose(f);

  void* out = malloc(expected_len);
  if (!out) die("malloc out");
  size_t got = ZSTD_decompress(out, expected_len, comp, (size_t)sz);
  free(comp);
  if (ZSTD_isError(got)) { free(out); fprintf(stderr, "ZSTD_decompress: %s\n", ZSTD_getErrorName(got)); exit(3); }
  if (got < expected_len) {
    void* p = realloc(out, got);
    if (p) out = p;
  }
  *out_len = got;
  return out;
}

// ------------ data structures

typedef struct { char* hash; int size; long long offset; } ChunkRef;

typedef struct {
  char* path;
  long long size;
  unsigned int mode;
  time_t mod_time;
  ChunkRef* chunks; int n_chunks; int cap_chunks;
} FileEntry;

typedef struct {
  char id[32], version[32], source_dir[PATH_MAX], host_name[256];
  time_t started, finished;
  FileEntry* files; int n_files; int cap_files;
  int total_files; long long total_bytes;
  int stored_chunks; int reused_chunks;
} Snapshot;

static void fe_push_chunk(FileEntry* fe, const char* hash, int size, long long offset) {
  if (fe->n_chunks == fe->cap_chunks) {
    fe->cap_chunks = fe->cap_chunks ? fe->cap_chunks * 2 : 8;
    fe->chunks = (ChunkRef*)realloc(fe->chunks, fe->cap_chunks * sizeof(ChunkRef));
    if (!fe->chunks) die("realloc chunks");
  }
  fe->chunks[fe->n_chunks].hash = strdup(hash);
  fe->chunks[fe->n_chunks].size = size;
  fe->chunks[fe->n_chunks].offset = offset;
  fe->n_chunks++;
}

static void snap_push_file(Snapshot* s, const FileEntry* fe_src) {
  if (s->n_files == s->cap_files) {
    s->cap_files = s->cap_files ? s->cap_files * 2 : 16;
    s->files = (FileEntry*)realloc(s->files, s->cap_files * sizeof(FileEntry));
    if (!s->files) die("realloc files");
  }
  s->files[s->n_files++] = *fe_src; // shallow move
}

// ------------ manifest IO

static void snapshot_to_json(const Snapshot* s, const char* repo) {
  json_t* j = json_object();
  json_object_set_new(j, "id",          json_string(s->id));
  json_object_set_new(j, "version",     json_string(s->version));
  json_object_set_new(j, "source_dir",  json_string(s->source_dir));
  json_object_set_new(j, "host_name",   json_string(s->host_name));
  json_object_set_new(j, "started",     json_integer(s->started));
  json_object_set_new(j, "finished",    json_integer(s->finished));
  json_object_set_new(j, "total_files", json_integer(s->total_files));
  json_object_set_new(j, "total_bytes", json_integer(s->total_bytes));
  json_object_set_new(j, "stored_chunks", json_integer(s->stored_chunks));
  json_object_set_new(j, "reused_chunks", json_integer(s->reused_chunks));

  json_t* files = json_array();
  for (int i = 0; i < s->n_files; ++i) {
    const FileEntry* fe = &s->files[i];
    json_t* jfe = json_object();
    json_object_set_new(jfe, "path", json_string(fe->path));
    json_object_set_new(jfe, "size", json_integer(fe->size));
    json_object_set_new(jfe, "mode", json_integer(fe->mode));
    json_object_set_new(jfe, "mod_time", json_integer(fe->mod_time));

    json_t* chunks = json_array();
    for (int c = 0; c < fe->n_chunks; ++c) {
      const ChunkRef* cr = &fe->chunks[c];
      json_t* jc = json_object();
      json_object_set_new(jc, "hash", json_string(cr->hash));
      json_object_set_new(jc, "size", json_integer(cr->size));
      json_object_set_new(jc, "offset", json_integer(cr->offset));
      json_array_append_new(chunks, jc);
    }
    json_object_set_new(jfe, "chunks", chunks);
    json_array_append_new(files, jfe);
  }
  json_object_set_new(j, "files", files);

  char mansdir[PATH_MAX], finalpath[PATH_MAX];
  path_join2(mansdir, sizeof(mansdir), repo, MANIFESTS_DIR);
  if (mkdirs(mansdir)) die("mkdir manifests");
  snprintf(finalpath, sizeof(finalpath), "%s/%s.json", mansdir, s->id);

  char* dump = json_dumps(j, JSON_INDENT(2));
  json_decref(j);
  atomic_write_bytes(finalpath, dump, strlen(dump));
  free(dump);
}

static json_t* load_manifest_json(const char* repo, const char* id) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s/%s.json", repo, MANIFESTS_DIR, id);
  json_error_t err;
  json_t* j = json_load_file(path, 0, &err);
  if (!j) {
    fprintf(stderr, "load manifest failed: %s (%s:%d)\n", err.text, err.source, err.line);
    exit(3);
  }
  return j;
}

// ------------ file collection (for parallelism)

typedef struct { char* full; char* rel; } FilePair;
typedef struct { FilePair* v; int n, cap; } FileVec;

static void fv_push(FileVec* fv, const char* full, const char* rel){
  if(fv->n==fv->cap){
    fv->cap = fv->cap? fv->cap*2:128;
    fv->v = (FilePair*)realloc(fv->v, fv->cap*sizeof(FilePair));
    if(!fv->v) die("realloc files");
  }
  fv->v[fv->n].full = strdup(full);
  fv->v[fv->n].rel  = strdup(rel);
  fv->n++;
}

static void walk_collect(const char* src_root, const char* rel, FileVec* out){
  char path[PATH_MAX];
  if (rel && rel[0]) snprintf(path, sizeof(path), "%s/%s", src_root, rel);
  else snprintf(path, sizeof(path), "%s", src_root);

  DIR* d = opendir(path);
  if (!d) { perror("opendir"); return; }

  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

    char child_rel[PATH_MAX];
    if (rel && rel[0]) snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name);
    else snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);

    char child_full[PATH_MAX];
    snprintf(child_full, sizeof(child_full), "%s/%s", src_root, child_rel);

    struct stat st;
    if (lstat(child_full, &st) != 0) { perror("lstat"); continue; }
    if (S_ISDIR(st.st_mode)) {
      walk_collect(src_root, child_rel, out);
    } else if (S_ISREG(st.st_mode)) {
      fv_push(out, child_full, child_rel);
    }
  }
  closedir(d);
}

// ------------ chunk claim (avoid duplicate compression across threads)

static int claim_chunk(const char* cpath) {
  char lock[PATH_MAX]; snprintf(lock, sizeof(lock), "%s.lock", cpath);
  int fd = open(lock, O_CREAT|O_EXCL|O_WRONLY, 0600);
  if (fd >= 0) { close(fd); return 1; }  // we own it
  return 0;                               // someone else is/was writing
}
static void release_claim(const char* cpath) {
  char lock[PATH_MAX]; snprintf(lock, sizeof(lock), "%s.lock", cpath);
  unlink(lock);
}

// ------------ worker pool

typedef struct {
  const char* repo;
  FileVec* files;
  atomic_int next;
  pthread_mutex_t snap_mu;
  Snapshot* snap;
} Work;

static void *worker_fn(void* arg){
  Work* w = (Work*)arg;

  // Reuse compression context and output buffer per thread
  size_t out_cap = ZSTD_compressBound(CHUNK_SIZE);
  void*  out_buf = malloc(out_cap);
  if (!out_buf) die("malloc out_buf");

  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  if (!cctx) die("ZSTD_createCCtx");
  if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1))) {
    fprintf(stderr, "zstd set level failed\n"); exit(3);
  }

  for(;;){
    int i = atomic_fetch_add(&w->next, 1);
    if(i >= w->files->n) break;

    const char* full = w->files->v[i].full;
    const char* rel  = w->files->v[i].rel;

    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

    FileEntry fe = {0};
    fe.path = strdup(rel);
    fe.size = (long long)st.st_size;
    fe.mode = (unsigned int)(st.st_mode & 0777);
    fe.mod_time = st.st_mtime;

    FILE* f = fopen(full, "rb");
    if (!f) { perror("open file"); free(fe.path); continue; }

    uint8_t* buf = (uint8_t*)malloc(CHUNK_SIZE);
    if (!buf) die("malloc in_buf");
    long long offset = 0;

    while (1) {
      size_t n = fread(buf, 1, CHUNK_SIZE, f);
      if (n == 0) break;

      char h[65]; sha256_hex(buf, n, h);
      char cpath[PATH_MAX]; chunk_path(cpath, w->repo, h);

      if (!file_exists(cpath)) {
        if (claim_chunk(cpath)) {
          size_t csz = ZSTD_compress2(cctx, out_buf, out_cap, buf, n);
          if (ZSTD_isError(csz)) { fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(csz)); exit(3); }
          atomic_write_bytes(cpath, out_buf, csz);
          release_claim(cpath);
          pthread_mutex_lock(&w->snap_mu);
          w->snap->stored_chunks += 1;
          pthread_mutex_unlock(&w->snap_mu);
        } else {
          // someone else is writing it; wait briefly for the file to appear
          for (int t=0; t<100 && !file_exists(cpath); ++t) usleep(1000);
          pthread_mutex_lock(&w->snap_mu);
          w->snap->reused_chunks += 1;
          pthread_mutex_unlock(&w->snap_mu);
        }
      } else {
        pthread_mutex_lock(&w->snap_mu);
        w->snap->reused_chunks += 1;
        pthread_mutex_unlock(&w->snap_mu);
      }

      fe_push_chunk(&fe, h, (int)n, offset);
      offset += (long long)n;
      if (n < CHUNK_SIZE) break;
    }

    fclose(f);
    free(buf);

    pthread_mutex_lock(&w->snap_mu);
    w->snap->total_files += 1;
    w->snap->total_bytes += fe.size;
    snap_push_file(w->snap, &fe); // ownership of fe members moves into snapshot
    pthread_mutex_unlock(&w->snap_mu);
  }

  ZSTD_freeCCtx(cctx);
  free(out_buf);
  return NULL;
}

// ------------ commands

static void now_id(char out[32]) {
  time_t t = time(NULL); struct tm tm;
  gmtime_r(&t, &tm);
  strftime(out, 32, "%Y%m%d-%H%M%S", &tm);
}

static void cmd_backup(const char* src, const char* repo) {
  Snapshot s = {0};
  now_id(s.id);
  snprintf(s.version, sizeof(s.version), "%s", VERSION_STR);
  if (!realpath(src, s.source_dir)) snprintf(s.source_dir, sizeof(s.source_dir), "%s", src);
  if (gethostname(s.host_name, sizeof(s.host_name)) != 0) snprintf(s.host_name, sizeof(s.host_name), "unknown");
  s.started = time(NULL);

  char cdir[PATH_MAX], mdir[PATH_MAX];
  path_join2(cdir, sizeof(cdir), repo, CHUNKS_DIR);
  path_join2(mdir, sizeof(mdir), repo, MANIFESTS_DIR);
  if (mkdirs(cdir) || mkdirs(mdir)) die("mkdir repo dirs");

  // collect all files
  FileVec fv = {0};
  walk_collect(src, "", &fv);

  // small thread pool
  int workers = cpu_count();
  if (workers < 1) workers = 1;

  Work w = { .repo=repo, .files=&fv, .next=0, .snap=&s };
  pthread_mutex_init(&w.snap_mu, NULL);

  pthread_t* tids = (pthread_t*)calloc(workers, sizeof(pthread_t));
  for (int t=0; t<workers; ++t) pthread_create(&tids[t], NULL, worker_fn, &w);
  for (int t=0; t<workers; ++t) pthread_join(tids[t], NULL);
  free(tids);
  pthread_mutex_destroy(&w.snap_mu);

  s.finished = time(NULL);
  snapshot_to_json(&s, repo);

  printf("Snapshot %s created. Files: %d, Bytes: %lld, Chunks: stored=%d reused=%d\n",
         s.id, s.total_files, s.total_bytes, s.stored_chunks, s.reused_chunks);

  // free collected file paths
  for (int i=0;i<fv.n;i++){ free(fv.v[i].full); free(fv.v[i].rel); }
  free(fv.v);
}

static void cmd_list(const char* repo) {
  char dir[PATH_MAX]; path_join2(dir, sizeof(dir), repo, MANIFESTS_DIR);
  DIR* d = opendir(dir);
  if (!d) { printf("No snapshots found.\n"); return; }
  struct dirent* e; bool any=false;
  while ((e = readdir(d)) != NULL) {
    const char* name = e->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st; if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

    size_t L = strlen(name);
    if (L < 6 || strcmp(name+L-5, ".json")) continue;

    any = true;
    char id[64]; strncpy(id, name, L-5); id[L-5] = '\0';
    json_t* j = load_manifest_json(repo, id);
    json_t* files   = json_object_get(j, "total_files");
    json_t* bytes   = json_object_get(j, "total_bytes");
    json_t* started = json_object_get(j, "started");
    printf("%s | files=%lld bytes=%lld started=%lld\n",
           id,
           files   ? (long long)json_integer_value(files)   : -1,
           bytes   ? (long long)json_integer_value(bytes)   : -1,
           started ? (long long)json_integer_value(started) : -1);
    json_decref(j);
  }
  closedir(d);
  if (!any) printf("No snapshots found.\n");
}

static void cmd_restore(const char* repo, const char* snap_id, const char* dst) {
  json_t* j = load_manifest_json(repo, snap_id);
  json_t* files = json_object_get(j, "files");
  if (!json_is_array(files)) { json_decref(j); die("manifest files"); }

  size_t idx; json_t* jfe;
  json_array_foreach(files, idx, jfe) {
    const char* path = json_string_value(json_object_get(jfe, "path"));
    long long size = json_integer_value(json_object_get(jfe, "size"));
    unsigned int mode = (unsigned int)json_integer_value(json_object_get(jfe, "mode"));
    time_t mod_time = (time_t)json_integer_value(json_object_get(jfe, "mod_time"));

    char outp[PATH_MAX]; snprintf(outp, sizeof(outp), "%s/%s", dst, path);
    if (ensure_dir_parent(outp)) die("mkdir dst parent");
    char tmp[PATH_MAX]; snprintf(tmp, sizeof(tmp), "%s.tmp", outp);

    FILE* out = fopen(tmp, "wb");
    if (!out) die("create temp out");
    fchmod(fileno(out), mode & 0777);

    json_t* jchunks = json_object_get(jfe, "chunks");
    size_t jci; json_t* jc;
    json_array_foreach(jchunks, jci, jc) {
      const char* h = json_string_value(json_object_get(jc, "hash"));
      int csz = (int)json_integer_value(json_object_get(jc, "size"));
      char cpath[PATH_MAX]; chunk_path(cpath, repo, h);
      size_t out_len = 0;
      void* data = zstd_decompress_file_exact(cpath, (size_t)csz, &out_len);
      if (fwrite(data, 1, out_len, out) != out_len) die("write out");
      free(data);
    }
    fclose(out);

#ifdef __APPLE__
    struct utimbuf ub; ub.actime = time(NULL); ub.modtime = mod_time; utime(tmp, &ub);
#else
    struct timespec ts[2]; ts[0].tv_sec = time(NULL); ts[0].tv_nsec=0; ts[1].tv_sec=mod_time; ts[1].tv_nsec=0;
    utimensat(AT_FDCWD, tmp, ts, 0);
#endif
    if (rename(tmp, outp) != 0) die("rename restore (replace)");
  }
  json_decref(j);
  printf("Restored snapshot %s to %s\n", snap_id, dst);
}

static void cmd_verify(const char* repo, const char* snap_id) {
  json_t* j = load_manifest_json(repo, snap_id);
  json_t* files = json_object_get(j, "files");
  if (!json_is_array(files)) { json_decref(j); die("manifest files"); }
  size_t idx; json_t* jfe;
  int missing = 0;
  json_array_foreach(files, idx, jfe) {
    json_t* jchunks = json_object_get(jfe, "chunks");
    size_t jci; json_t* jc;
    json_array_foreach(jchunks, jci, jc) {
      const char* h = json_string_value(json_object_get(jc, "hash"));
      char cpath[PATH_MAX]; snprintf(cpath, sizeof(cpath), "%s/%s/%c%c/%s.zst",
                                     repo, CHUNKS_DIR, h[0], h[1], h);
      if (!file_exists(cpath)) { printf("missing: %s\n", h); missing++; }
    }
  }
  json_decref(j);
  if (missing == 0) { printf("Snapshot %s: all chunks present.\n", snap_id); }
  else { printf("Snapshot %s: %d missing chunks.\n", snap_id, missing); exit(4); }
}

// ------------ arg parsing and main

static const char* get_arg(int argc, char** argv, const char* name) {
  for (int i=2;i+1<argc;i++) if (strcmp(argv[i], name)==0) return argv[i+1];
  return NULL;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("minibackup_c %s\nCommands: backup | list | restore | verify\n", VERSION_STR);
    return 1;
  }
  const char* cmd = argv[1];
  if (strcmp(cmd, "backup")==0) {
    const char* src = get_arg(argc, argv, "--src");
    const char* repo= get_arg(argc, argv, "--repo");
    if (!src || !repo) { fprintf(stderr, "backup requires --src and --repo\n"); return 2; }
    cmd_backup(src, repo);
  } else if (strcmp(cmd, "list")==0) {
    const char* repo= get_arg(argc, argv, "--repo");
    if (!repo) { fprintf(stderr, "list requires --repo\n"); return 2; }
    cmd_list(repo);
  } else if (strcmp(cmd, "restore")==0) {
    const char* repo= get_arg(argc, argv, "--repo");
    const char* id  = get_arg(argc, argv, "--snapshot");
    const char* dst = get_arg(argc, argv, "--dst");
    if (!repo || !id || !dst) { fprintf(stderr, "restore requires --repo --snapshot --dst\n"); return 2; }
    cmd_restore(repo, id, dst);
  } else if (strcmp(cmd, "verify")==0) {
    const char* repo= get_arg(argc, argv, "--repo");
    const char* id  = get_arg(argc, argv, "--snapshot");
    if (!repo || !id) { fprintf(stderr, "verify requires --repo --snapshot\n"); return 2; }
    cmd_verify(repo, id);
  } else {
    fprintf(stderr, "unknown command: %s\n", cmd);
    return 2;
  }
  return 0;
}
