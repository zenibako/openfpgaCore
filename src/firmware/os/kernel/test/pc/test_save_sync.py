"""Exercise production save flushing and syscall dispatch against a mock bridge.

CRAM and mock SD are separate arrays; exit writeback deliberately does nothing.
The test extracts real routines, as the adjacent mmap fixture does.
"""
from pathlib import Path
import os
import re
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[6]
kernel = (repo / 'src/firmware/os/kernel/syscall.c').read_text()
hal = (repo / 'src/firmware/os/targets/pocket/save.c').read_text()
mister = (repo / 'src/firmware/os/targets/mister/save.c').read_text()
if os.environ.get('OF_SAVE_SYNC_BASELINE'):
    revision = os.environ['OF_SAVE_SYNC_BASELINE']
    kernel = subprocess.check_output(['git','show',revision+':src/firmware/os/kernel/syscall.c'],cwd=repo,text=True)
    hal = subprocess.check_output(['git','show',revision+':src/firmware/os/targets/pocket/save.c'],cwd=repo,text=True)

def function(source, marker):
    start = source.index(marker)
    if source.find(';', start, source.index('{', start)) >= 0:
        start = source.index(marker, start + len(marker))
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

hal_pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SAVE_SLOT_SIZE 262144u
#define OF_TARGET_SAVE_WRITEBACK_CHUNK_SIZE 65536u
static uint8_t cram[SAVE_SLOT_SIZE], sd[SAVE_SLOT_SIZE];
static uint32_t cram0_cpu_hold_count, metadata_size, sd_size, clock_ms;
static unsigned writes, polls, pending, copy_pending;
static int mode, fail_commit, fail_write, fail_readback, bad_readback, resubmit;
typedef struct { uint32_t capacity, bridge_addr; } nvslot_map_t;
static int of_nvslot_is_supported(uint32_t id) { return id == 10; }
static int nvslot_map(uint32_t id, nvslot_map_t *map) {
    if (id != 10) return -1;
    map->capacity=SAVE_SLOT_SIZE; map->bridge_addr=0x20100000u; return 0;
}
static int nvslot_commit_size(uint32_t id, const nvslot_map_t *map,
                              uint32_t size, uint32_t *entry) {
    assert(id==10 && map->capacity==SAVE_SLOT_SIZE);
    if(fail_commit) return -1;
    metadata_size=size; *entry=9; return 0;
}
static int of_file_datatable_word(uint32_t word, uint32_t *got) {
    assert(word==19); if(fail_readback)return -1;
    *got=metadata_size+(bad_readback?1:0); return 0;
}
static void of_term_printf(const char *fmt, ...) { (void)fmt; }
static void of_timer_delay_us(uint32_t us) { (void)us; }
static uint32_t of_timer_get_ms(void) { return clock_ms++; }
static int of_file_async_busy(void) { return pending != 0; }
static int of_file_cram0_quiet(void) { return !copy_pending; }
static int of_file_async_poll(void) {
    polls++;
    if(pending) pending--;
    if(!pending) copy_pending=0;
    if(resubmit) { pending=1; mode=0; }
    return 0;
}
static void cram0_release_to_bridge(void) { mode=0; }
static void cram0_acquire_cpu(void) { mode=1; }
static int of_file_slot_write_chunked(uint32_t id, uint32_t offset,
                                    uint32_t address, uint32_t size, uint32_t chunk) {
    assert(id==10 && offset==0 && address==0x20100000u && mode==0);
    assert(!pending && !copy_pending && chunk>0 && size<=SAVE_SLOT_SIZE);
    if(fail_write) { pending=1; return -1; } /* accepted, but host timed out */
    for(uint32_t n=0;n<size;n+=chunk) {
        uint32_t count=size-n; if(count>chunk) count=chunk;
        memcpy(sd+n,cram+n,count); writes++;
    }
    if(!size) writes++;
    sd_size=size; return 0;
}
'''
hal_post = r'''
static void reset(unsigned holds) {
    cram0_cpu_hold_count=holds; mode=holds?1:0;
    metadata_size=sd_size=clock_ms=writes=polls=pending=copy_pending=0;
    fail_commit=fail_write=fail_readback=bad_readback=resubmit=0;
    for(unsigned i=0;i<SAVE_SLOT_SIZE;i++) cram[i]=(uint8_t)(i*37+11);
    memset(sd,0,sizeof(sd));
}
int main(void) {
    for(unsigned holds=0;holds<=2;holds++) {
        reset(holds);
        assert(of_nvslot_flush_size(10,63215)==0);
        assert(cram0_cpu_hold_count==holds && mode==(holds?1:0));
        assert(sd_size==63215 && memcmp(cram,sd,sd_size)==0);
    }
    reset(2); pending=3;copy_pending=1;
    assert(of_nvslot_flush_size(10,131075)==0 && polls==3 && writes==3);
    assert(cram0_cpu_hold_count==2 && mode==1 && memcmp(cram,sd,131075)==0);
    reset(1);copy_pending=1;
    assert(of_nvslot_flush_size(10,0)==0 && polls==1 && sd_size==0 && writes==1);
    reset(2);resubmit=1;pending=1;
    assert(of_nvslot_flush_size(10,63215)<0 && polls>=1000 && polls<=2001);
    assert(!writes && cram0_cpu_hold_count==2 && mode==0 && pending);
    reset(2);fail_commit=1;
    assert(of_nvslot_flush_size(10,63215)<0 && !writes && cram0_cpu_hold_count==2 && mode==1);
    reset(2);fail_write=1;
    assert(of_nvslot_flush_size(10,63215)<0 && !sd_size && cram0_cpu_hold_count==2 && mode==0 && pending);
    reset(2);fail_readback=1;
    assert(of_nvslot_flush_size(10,63215)<0 && !writes && cram0_cpu_hold_count==2 && mode==1);
    reset(2);bad_readback=1;
    assert(of_nvslot_flush_size(10,63215)<0 && !writes && cram0_cpu_hold_count==2 && mode==1);
    reset(1);assert(of_nvslot_flush_size(99,5)<0 && !writes && cram0_cpu_hold_count==1 && mode==1);
    reset(1);assert(of_nvslot_flush_size(10,SAVE_SLOT_SIZE+10)==0 && sd_size==SAVE_SLOT_SIZE);
    puts("PASS real HAL sync: separate SD bytes, nested holds, DMA/copy drain, bounded resubmission, errors, zero and chunks");
}
'''

kernel_pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ENOSYS 38
#define EBADF 9
#define EINVAL 22
#define EIO 5
#define EACCES 13
#define EFBIG 27
#define FD_STDOUT 1
#define FD_STDERR 2
#define O_ACCMODE 3
#define O_WRONLY 1
#define O_APPEND 1024
#define O_TRUNC 512
#define FD_KIND_NV_FILE 1
#define NV_CACHE_SLOTS 1
#define SAVE_SLOT_SIZE 65536u
#define FD_FIRST_FREE 3
#define MAX_FDS 8
#define SYS_fsync 82
#define SYS_fdatasync 83
typedef struct { int in_use,is_save,dirty,nv_cache_idx,flags,kind,save_slot; uint32_t slot_id; uint64_t size,offset; } fd_entry_t;
static fd_entry_t fd_table[MAX_FDS];
static uint8_t cram[65536],sd[65536];
static uint32_t stored_size,metadata_size;
static uint32_t save_size_cache[NV_CACHE_SLOTS];
static uint8_t save_size_known[NV_CACHE_SLOTS], save_zero_authoritative[NV_CACHE_SLOTS];
#define latest save_size_cache[0]
static unsigned flushes;
static int fail_flush;
static long of_file_size(uint32_t id) { assert(id==10);return metadata_size; }
static int of_nvslot_flush_size(uint32_t id,uint32_t size) {
 assert(id==10 && size<=sizeof(sd)); flushes++;
 if(fail_flush)return -1;
 memcpy(sd,cram,size);stored_size=size;return 0;
}
'''
kernel_pre += r'''
static void of_term_putchar(char ch) { (void)ch; }
static uint32_t of_nvslot_capacity(uint32_t id) { assert(id==10);return sizeof(cram); }
static int of_nvslot_write(uint32_t id,const void *buf,uint32_t off,uint32_t size) {
    assert(id==10 && off+size<=sizeof(cram));memcpy(cram+off,buf,size);return size;
}
static int of_nvslot_set_size(uint32_t id,uint32_t size) { assert(id==10);metadata_size=size;return 0; }
static void of_save_begin_cpu(void) {}
static int nv_cache_index_from_data_id(uint32_t id) { return id==10?0:-1; }
static int of_nvslot_is_supported(uint32_t id) { return id==10; }
static int save_slot_from_data_id(uint32_t id) { assert(id==10);return 0; }
static uint32_t nv_infer_text_size(uint32_t id) { (void)id;return 0; }
static int open_flags_read_only(long flags) { return !(flags & O_ACCMODE); }
static void of_save_end_cpu(void) {}
'''
kernel_pre += ''.join(function(kernel, marker) for marker in ('static uint32_t save_clamp_size(', 'static void save_size_cache_store(', 'static void save_size_cache_invalidate(', 'static uint32_t nv_current_size('))
if 'static long sys_fsync(' in kernel:
    routine = function(kernel, 'static long sys_fsync(')
    cases = re.search(r'    case SYS_fsync:.*?\n(?:    case SYS_fdatasync:.*?\n)?', kernel).group()
else:
    routine = ''
    cases = ''
routine += function(kernel, 'static long open_nv_fd(') + function(kernel, 'static long sys_write(') + function(kernel, 'static long sys_close(')
dispatch = 'static long dispatch(long nr,long a0) { switch(nr) {\n'+cases+'default:return -ENOSYS;}}\n'
kernel_post = r'''
int main(void) {
    memset(cram,0x5a,sizeof(cram));memset(sd,0x11,sizeof(sd));
    save_size_cache_store(0,63215);
    fd_table[3].in_use=fd_table[3].is_save=fd_table[3].dirty=1;
    fd_table[3].slot_id=10;fd_table[3].size=63215;
    fd_table[4]=fd_table[3];fd_table[4].size=63211;
    assert(dispatch(SYS_fsync,4)==0);
    assert(stored_size==63215 && !memcmp(cram,sd,stored_size));
    assert(fd_table[3].size==63215 && fd_table[4].size==63215);
    assert(!fd_table[3].dirty && !fd_table[4].dirty);
    assert(dispatch(SYS_fdatasync,3)==0 && flushes==2);
    assert(dispatch(SYS_fsync,99)==-EBADF && flushes==2);
    fd_table[5].in_use=1;assert(dispatch(SYS_fsync,5)==-EINVAL);
    fd_table[3].dirty=1;fail_flush=1;
    assert(dispatch(SYS_fsync,3)==-EIO && fd_table[3].dirty);
    fail_flush=0;save_size_cache_store(0,100);
    memset(fd_table,0,sizeof(fd_table));
    fd_table[3].in_use=fd_table[3].is_save=1;
    fd_table[3].slot_id=10;fd_table[3].size=100;fd_table[3].flags=O_WRONLY;
    fd_table[4]=fd_table[3];
    unsigned char bytes[200];memset(bytes,0x77,sizeof(bytes));
    assert(sys_write(3,(long)bytes,200)==200);
    assert(sys_write(4,(long)bytes,1)==1);
    assert(dispatch(SYS_fsync,3)==0 && stored_size==200);
    assert(!memcmp(cram,sd,200));
    /* Append uses shared EOF even when this descriptor predates growth. */
    fd_table[4].size=100;fd_table[4].flags=O_WRONLY|O_APPEND;
    assert(sys_write(4,(long)bytes,1)==1 && latest==201 && cram[200]==0x77);
    /* An older dirty descriptor must not shrink a later write on close. */
    fd_table[3].size=100;fd_table[3].dirty=1;
    assert(sys_close(3)==0 && metadata_size==201 && latest==201);
    /* Shared truncation must win over an older descriptor's size. */
    save_size_cache_store(0,0);fd_table[4].dirty=1;
    assert(sys_close(4)==0 && metadata_size==0 && latest==0);
    /* Unknown zero on a clean reader must not truncate populated SD. */
    memset(fd_table,0,sizeof(fd_table));
    metadata_size=0;stored_size=63215;save_size_cache_invalidate(0);
    fd_table[3].in_use=1;
    assert(open_nv_fd(3,10,0)==3 && fd_table[3].size==sizeof(cram));
    unsigned before_flush=flushes;
    assert(dispatch(SYS_fsync,3)==0 && stored_size==63215 && flushes==before_flush);
    assert(sys_close(3)==0);
    /* Explicit truncate is authoritative even when its requested size is zero. */
    fd_table[3]=(fd_entry_t){.in_use=1,.flags=O_WRONLY|O_TRUNC};
    assert(open_nv_fd(3,10,O_WRONLY|O_TRUNC)==3);
    fail_flush=1;
    assert(dispatch(SYS_fsync,3)==-EIO && fd_table[3].dirty);
    fail_flush=0;
    assert(dispatch(SYS_fdatasync,3)==0 && stored_size==0 && !fd_table[3].dirty);
    assert(sys_close(3)==0);
    /* Explicit empty save must also survive closing before synchronization. */
    fd_table[3]=(fd_entry_t){.in_use=1,.flags=O_WRONLY|O_TRUNC};
    assert(open_nv_fd(3,10,O_WRONLY|O_TRUNC)==3 && sys_close(3)==0);
    fd_table[4]=(fd_entry_t){.in_use=1};
    assert(open_nv_fd(4,10,0)==4);
    before_flush=flushes;
    assert(dispatch(SYS_fsync,4)==0 && stored_size==0 && flushes==before_flush+1);
    assert(sys_close(4)==0);
    /* Closing without fsync must not prevent a reopened clean fd from syncing. */
    fd_table[3]=(fd_entry_t){.in_use=1,.flags=O_WRONLY};
    assert(open_nv_fd(3,10,O_WRONLY)==3);
    assert(sys_write(3,(long)bytes,200)==200 && sys_close(3)==0);
    assert(stored_size==0 && metadata_size==200);
    fd_table[4]=(fd_entry_t){.in_use=1};
    assert(open_nv_fd(4,10,0)==4 && !fd_table[4].dirty);
    assert(dispatch(SYS_fsync,4)==0 && stored_size==200 && !memcmp(cram,sd,200));
    puts("PASS real fsync dispatch: SD survives missing exit writeback, latest shared size, dirty/error handling");
}
'''
mister_pre = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { int unused; } FIL;
#define FR_OK 0
static FIL backing;
static int mounted=1, sync_rc, commit_rc, locks, opens, syncs;
static int of_nvslot_set_size(uint32_t id,uint32_t size) { assert(id==10&&size==100);return commit_rc; }
static void mister_fs_enter(void) { assert(locks==0);locks++; }
static void mister_fs_exit(void) { assert(locks==1);locks--; }
static FIL *mister_file_open_slot(uint32_t id,int write) { assert(id==10&&write==1&&locks==1);opens++;return mounted?&backing:0; }
static int f_sync(FIL *fil) { assert(fil==&backing&&locks==1);syncs++;return sync_rc; }
"""
mister_post = r"""
int main(void) {
    mounted=0;assert(of_nvslot_flush_size(10,100)<0 && !locks && !syncs);
    mounted=1;sync_rc=1;assert(of_nvslot_flush_size(10,100)<0 && !locks && syncs==1);
    sync_rc=0;assert(of_nvslot_flush_size(10,100)==0 && !locks && syncs==2);
    commit_rc=-1;int before=opens;
    assert(of_nvslot_flush_size(10,100)<0 && opens==before && !locks);
    puts("PASS real MiSTer flush: missing backing file, sync failure, success, metadata failure and lock release");
}
"""

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = {**os.environ, 'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1'}
failed = False
with tempfile.TemporaryDirectory(prefix='of-save-sync-') as directory:
    root = Path(directory)
    for name, source in [('hal',hal_pre+function(hal,'int of_nvslot_flush_size(')+hal_post),
                         ('syscall',kernel_pre+routine+dispatch+kernel_post),
                         ('mister',mister_pre+function(mister,'int of_nvslot_flush_size(')+mister_post)]:
        path=root/(name+'.c');path.write_text(source)
        subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Wno-unused-function',
                        '-fsanitize=address,undefined',str(path),'-o',str(root/name)],check=True)
        result=subprocess.run([str(root/name)],env=env,capture_output=True,text=True)
        print(name,'exit',result.returncode,result.stdout,result.stderr)
        failed |= result.returncode != 0
raise SystemExit(1 if failed else 0)
