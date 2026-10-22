/* snowfs.c - SnowFS: sistema de arquivos somente leitura sobre o DVD-ROM (formato em src/abi.h / src/sffs.h) */
#include "snowos.h"

#define FS_MAX_OPEN 8

struct FsEntry { char name[FS_NAME_MAX]; unsigned lba, size, flags, reserved; };
struct FsFile { int used; unsigned lba, size, pos; };

static Sem fs_lock, fs_irq;
static int fs_ready;
static struct FsEntry *fs_dir;
static int fs_nfiles;
static struct FsFile fs_files[FS_MAX_OPEN];
static unsigned char fs_bounce[DVD_SECTOR];

/* lê 'n' setores para 'dst' (DMA do DVD; a D-cache não é coerente, então descarta as linhas do destino) */
static int fs_dvd_read(unsigned lba, unsigned n, void *dst) {
  unsigned st;
  os_cache_flush((unsigned)dst, n * DVD_SECTOR); os_cache_invalidate((unsigned)dst, n * DVD_SECTOR);
  fs_irq.count = 0;
  REG(DVD_BASE + DVD_STATUS) = 6;                              /* limpa DONE/ERRO */
  REG(DVD_BASE + DVD_LBA) = lba; REG(DVD_BASE + DVD_COUNT) = n; REG(DVD_BASE + DVD_DST) = (unsigned)dst;
  REG(DVD_BASE + DVD_CMD) = 1;
  for (;;) {
    st = REG(DVD_BASE + DVD_STATUS);
    if (!(st & 1)) break;
    sem_wait_timeout(&fs_irq, 50);
  }
  os_cache_invalidate((unsigned)dst, n * DVD_SECTOR);
  return (st & 4) ? -1 : 0;
}

static int fs_streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

int fs_mount(void) {
  unsigned *sb, nfiles, dir_lba, dir_sectors;
  if (!fs_ready) { sem_init(&fs_lock, 1); sem_init(&fs_irq, 0); os_irq_attach(11, &fs_irq); REG(DVD_BASE + DVD_CTRL) = 1; fs_ready = 1; }
  sem_wait(&fs_lock);
  if (fs_dir) { os_free(fs_dir); fs_dir = 0; } fs_nfiles = 0;
  if (!(REG(DVD_BASE + DVD_STATUS) & 8) || fs_dvd_read(0, 1, fs_bounce)) { sem_post(&fs_lock); return -1; }
  sb = (unsigned *)fs_bounce;
  if (sb[0] != 0x53464653 || sb[1] != 1) { sem_post(&fs_lock); return -1; }
  nfiles = sb[2]; dir_lba = sb[3]; dir_sectors = sb[4];
  fs_dir = (struct FsEntry *)os_malloc(dir_sectors * DVD_SECTOR);
  if (!fs_dir || nfiles * 64 > dir_sectors * DVD_SECTOR || fs_dvd_read(dir_lba, dir_sectors, fs_dir)) { if (fs_dir) os_free(fs_dir); fs_dir = 0; sem_post(&fs_lock); return -1; }
  fs_nfiles = (int)nfiles;
  { int i; for (i = 0; i < FS_MAX_OPEN; i++) fs_files[i].used = 0; }
  sem_post(&fs_lock);
  return 0;
}

int fs_count(void) { return fs_nfiles; }
const char *fs_name(int i) { return (i >= 0 && i < fs_nfiles) ? fs_dir[i].name : NULL; }
unsigned fs_size_at(int i) { return (i >= 0 && i < fs_nfiles) ? fs_dir[i].size : 0; }

int fs_open(const char *path) {
  int i, h = -1;
  if (!fs_dir) return -1;
  sem_wait(&fs_lock);
  for (i = 0; i < fs_nfiles; i++) {
    if (!fs_streq(fs_dir[i].name, path)) continue;
    { int k; for (k = 0; k < FS_MAX_OPEN; k++) if (!fs_files[k].used) { h = k; break; } }
    if (h >= 0) { fs_files[h].used = 1; fs_files[h].lba = fs_dir[i].lba; fs_files[h].size = fs_dir[i].size; fs_files[h].pos = 0; }
    break;
  }
  sem_post(&fs_lock);
  return h;
}

int fs_read(int h, void *dst, unsigned n) {
  struct FsFile *f; unsigned char *d = (unsigned char *)dst; unsigned done = 0;
  if (h < 0 || h >= FS_MAX_OPEN || !fs_files[h].used) return -1;
  f = &fs_files[h];
  if (n > f->size - f->pos) n = f->size - f->pos;
  sem_wait(&fs_lock);
  while (done < n) {
    unsigned off = f->pos % DVD_SECTOR, lba = f->lba + f->pos / DVD_SECTOR, left = n - done, k;
    if (off == 0 && left >= DVD_SECTOR) {                      /* setores inteiros direto no destino */
      k = left / DVD_SECTOR;
      if (fs_dvd_read(lba, k, d + done)) { sem_post(&fs_lock); return -1; }
      k *= DVD_SECTOR;
    } else {                                                   /* parcial: passa pelo buffer de setor */
      k = DVD_SECTOR - off; if (k > left) k = left;
      if (fs_dvd_read(lba, 1, fs_bounce)) { sem_post(&fs_lock); return -1; }
      { unsigned i; for (i = 0; i < k; i++) d[done + i] = fs_bounce[off + i]; }
    }
    done += k; f->pos += k;
  }
  sem_post(&fs_lock);
  return (int)done;
}

int fs_seek(int h, unsigned pos) {
  if (h < 0 || h >= FS_MAX_OPEN || !fs_files[h].used || pos > fs_files[h].size) return -1;
  fs_files[h].pos = pos; return 0;
}
unsigned fs_tell(int h) { return (h >= 0 && h < FS_MAX_OPEN && fs_files[h].used) ? fs_files[h].pos : 0; }
unsigned fs_size(int h) { return (h >= 0 && h < FS_MAX_OPEN && fs_files[h].used) ? fs_files[h].size : 0; }
void fs_close(int h) { if (h >= 0 && h < FS_MAX_OPEN) fs_files[h].used = 0; }

int fs_load(const char *path, void *dst, unsigned max) {
  int h = fs_open(path), r;
  if (h < 0) return -1;
  r = fs_read(h, dst, max); if (r >= 0) r = (int)fs_size(h) > (int)max ? (int)max : (int)fs_size(h);
  fs_close(h); return r;
}
