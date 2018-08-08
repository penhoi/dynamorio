#include <sys/types.h>
#include <sys/mman.h>

/* for open */
#include <sys/stat.h>
#include <fcntl.h>
#include "../globals.h"
#include "../hashtable.h"
#include "../native_exec.h"
#include <string.h>
#include <unistd.h> /* for write and usleep and _exit */
#include <limits.h>


#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <syslog.h>             /* vsyslog */
#include "../vmareas.h"
#ifdef RCT_IND_BRANCH
# include "../rct.h"
#endif
#ifdef LINUX
# include "include/syscall.h"            /* our own local copy */
#else
# include <sys/syscall.h>
#endif
#include "../module_shared.h"
#include "os_private.h"
#include "../synch.h"
#include "memquery.h"
#include "ksynch.h"

#include "sgx_vma.h"

/* add new item between llp and lln */
void list_add(list_t *llp, list_t *lln, list_t *ll)
{
    //YPHASSERT(llp != lln);
    /* update the link */
    ll->prev = llp;
    ll->next = lln;

    llp->next = ll;
    lln->prev = ll;
}

/* delete item from its list */
void list_del(list_t *ll)
{
    list_t *llp;
    list_t *lln;

    llp = ll->prev;
    lln = ll->next;
    YPHASSERT(llp != lln);

    /* update the link */
    lln->prev = llp;
    llp->next = lln;
}

#define SGX_PAGE_SIZE 4096

/* simulate the task_struct::fs[] */
#define SGX_PROCMAPS_MAX_FILE 20
char procmaps_cmt[SGX_PROCMAPS_MAX_FILE][64];

void sgx_vma_set_cmt(ulong fd, const char *fname)
{
    if (fd >= SGX_PROCMAPS_MAX_FILE)
        return;

    if (strlen(fname) > SGX_VMA_CMT_LEN)
        return;

    strncpy(procmaps_cmt[fd], fname, SGX_VMA_CMT_LEN);
}

void sgx_vma_get_cmt(ulong fd, char *buffer)
{
    if (fd >= SGX_PROCMAPS_MAX_FILE)
        return;

    strncpy(buffer, procmaps_cmt[fd], SGX_VMA_CMT_LEN);
}



byte* sgx_vm_base = NULL;
byte* ext_vm_base = NULL;
sgx_mm_t sgxmm;

bool sgx_mm_within(byte* addr, size_t len)
{
    return (addr > sgx_vm_base && addr+len < sgx_vm_base+SGX_BUFFER_SIZE);
}

byte* sgx_mm_itn2ext(byte* itn_addr)
{
    //YPHASSERT(itn_addr > sgx_vm_base && itn_addr < sgx_vm_base+SGX_BUFFER_SIZE);
    if (itn_addr > sgx_vm_base && itn_addr < sgx_vm_base+SGX_BUFFER_SIZE)
        return (itn_addr - sgx_vm_base) + ext_vm_base;
    else
        return itn_addr;
}

byte* sgx_mm_ext2itn(byte* ext_addr)
{
    //YPHASSERT(ext_addr > ext_vm_base && ext_addr < ext_vm_base+SGX_BUFFER_SIZE);
    if (ext_addr > ext_vm_base && ext_addr < ext_vm_base+SGX_BUFFER_SIZE)
        return (ext_addr - ext_vm_base) + sgx_vm_base;
    else
        return ext_addr;
}



/* The memroy layout when Dynamorio is executed */
#define dr_code_size    0x3ae000
#define vvar_size       0x3000
#define vdso_size       0x2000
#define dr_data_size    0x46000
#define heap_size       0x3f000
#define stack_size      0x21000
#define vsyscall_size   0x1000

#define dr_code_start_s1    (byte*)0x7ffff79cc000
#define dr_code_end_s1      (byte*)(dr_code_start_s1 + dr_code_size)
#define vvar_start          (byte*)(dr_code_end_s1 + 0x1fb000)
#define vvar_end            (byte*)(vvar_start + vvar_size)
#define vdso_start          (byte*)(vvar_end)
#define vdso_end            (byte*)(vdso_start + vdso_size)
#define dr_data_start_s1    (byte*)(vdso_end)
#define dr_data_end_s1      (byte*)(dr_data_start_s1 + dr_data_size)
#define heap_start          (byte*)(dr_data_end_s1)
#define heap_end            (byte*)(heap_start + heap_size)
#define stack_start         (byte*)0x7ffffffde000
#define stack_end           (byte*)(stack_start + stack_size)
#define vsyscall_start      (byte*)0xffffffffff600000
#define vsyscall_end        (byte*)(vsyscall_start + vsyscall_size)

/* The memroy layout if Dynamorio is reloaed */
#define dr_hole1_size       0x200000
#define dr_hole2_size       0x3f000
#define dr_hole3_size       0x1000

#define dr_code_start_s2    (byte*)0x71000000
#define dr_code_end_s2      (byte*)(dr_code_start_s2 + dr_code_size)
#define dr_hole1_start_s2   (byte*)(dr_code_end_s2)
#define dr_hole1_end_s2     (byte*)(dr_hole1_start_s2 + dr_hole1_size)
#define dr_data_start_s2    (byte*)(dr_hole1_end_s2)
#define dr_data_end_s2      (byte*)(dr_data_start_s2 + dr_data_size)
#define dr_hole2_start_s2   (byte*)(dr_data_end_s2)
#define dr_hole2_end_s2     (byte*)(dr_hole2_start_s2 + dr_hole2_size)
#define dr_hole3_start_s2   (byte*)(dr_hole2_end_s2)
#define dr_hole3_end_s2     (byte*)(dr_hole3_start_s2 + dr_hole3_size)

#define sgx_mm_size         0x4000000
#define sgx_mm_start        (byte*)0x7fff00000000
#define sgx_mm_end          (byte*)(sgx_mm_start + sgx_mm_size)


#define DR_PATH "/home/yph/project/dynamorio.org/debug/lib64/debug/libdynamorio.so"
sgx_vm_area_t vma_list_exec[] = {
    {dr_code_start_s1, dr_code_end_s1, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, {NULL, NULL}, DR_PATH},
    {vvar_start, vvar_end, NULL, PROT_READ, 0, 0, 0, {NULL, NULL}, "[vvar]"},
    {vdso_start, vdso_end, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, {NULL, NULL}, "[vdso]"},
    {dr_data_start_s1, dr_data_end_s1, NULL, PROT_READ|PROT_WRITE, 0, 0, dr_code_size, {NULL, NULL}, DR_PATH},
    {heap_start, heap_end, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, {NULL, NULL}, "[heap]"},
    {stack_start, stack_end, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, {NULL, NULL}, "[stack]"},
    {vsyscall_start, vsyscall_end, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, {NULL, NULL}, "[vsyscall]"},
    {(byte*)NULL, (byte*)NULL, NULL, 0, 0, 0, 0, {NULL, NULL}, ""}
};

sgx_vm_area_t vma_list_reload[] = {
    {dr_code_start_s2, dr_code_end_s2, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, {NULL, NULL}, DR_PATH},
    {dr_hole1_start_s2, dr_hole1_end_s2, NULL, PROT_NONE, 0, 0, 0, {NULL, NULL}, ""},
    {dr_data_start_s2, dr_data_end_s2, NULL, PROT_READ|PROT_WRITE, 0, 0, dr_code_size, {NULL, NULL}, DR_PATH},
    {dr_hole2_start_s2, dr_hole2_end_s2, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, {NULL, NULL}, ""},
    {dr_hole3_start_s2, dr_hole3_end_s2, NULL, PROT_NONE, 0, 0, 0, {NULL, NULL}, ""},
    {dr_code_start_s1, dr_code_end_s1, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, {NULL, NULL}, DR_PATH},
    {sgx_mm_start, sgx_mm_end, NULL, PROT_READ|PROT_WRITE|PROT_EXEC, 0, 0, 0, {NULL, NULL}, "[sgxmm]"},
    {vvar_start, vvar_end, NULL, PROT_READ, 0, 0, 0, {NULL, NULL}, "[vvar]"},
    {vdso_start, vdso_end, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, {NULL, NULL}, "[vdso]"},
    {dr_data_start_s1, dr_data_end_s1, NULL, PROT_READ|PROT_WRITE, 0, 0, dr_code_size, {NULL, NULL}, DR_PATH},
    {heap_start, heap_end, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, {NULL, NULL}, "[heap]"},
    {stack_start, stack_end, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, {NULL, NULL}, "[stack]"},
    {vsyscall_start, vsyscall_end, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, {NULL, NULL}, "[vsyscall]"},
    {(byte*)NULL, (byte*)NULL, NULL, 0, 0, 0, 0, {NULL, NULL}, ""}
};


//forward declaration

sgx_vm_area_t* _sgx_vma_alloc(list_t* llp, list_t* lln);
void _sgx_vma_free(sgx_vm_area_t* vma);
static void _sgx_vma_fill(sgx_vm_area_t* vma, byte* ext_addr, size_t len, ulong prot, int fd, ulong offs);

/* init sgx_mm: first should be 1 for true, 0 for reload and -1 for unit debugging */
void sgx_mm_init(int first)
{
    sgx_vm_area_t *vma = NULL;
    sgx_vm_area_t *add = NULL;
    list_t *ll = NULL;
    int idx;

    YPHASSERT(SGX_VMA_MAX_CNT > 2);
    for (idx = 0, vma = sgxmm.slots; idx < SGX_VMA_MAX_CNT; idx++, vma++) {
        ll = &vma->ll;
        ll->prev = NULL;    /* set prev to NULL if not used */
        if(idx == SGX_VMA_MAX_CNT - 1) {
            ll->next = NULL;
        }
        else {
            ll->next = &(sgxmm.slots[idx+1].ll);
        }
    }
    sgxmm.un = &(sgxmm.slots[0].ll);
    sgxmm.in.prev = &sgxmm.in;
    sgxmm.in.next = &sgxmm.in;

    sgxmm.nin = 0;
    sgxmm.nun = SGX_VMA_MAX_CNT;

    /* Allocate a big buffer for loading target program into SGX*/
    if (first == true) /* please don't change it */ {
        sgx_vm_base = (byte*)dynamorio_syscall(IF_MACOS_ELSE(SYS_mmap, IF_X64_ELSE(SYS_mmap, SYS_mmap2)), 6,
                SGX_BUFFER_BASE,
                SGX_BUFFER_SIZE,
                PROT_READ|PROT_WRITE|PROT_EXEC,
                MAP_PRIVATE|MAP_ANONYMOUS,
                -1, 0);
        vma = vma_list_exec;
    }
    else {
        sgx_vm_base = (byte*)SGX_BUFFER_BASE;
        vma = vma_list_reload;
    }


    YPHASSERT(sgx_vm_base == (byte*)SGX_BUFFER_BASE);
    sgxmm.vm_base = sgx_vm_base;
    sgxmm.vm_size = SGX_BUFFER_SIZE;
    ext_vm_base = (app_pc)0x7ffff0000000;


    /* Our unit test not wants to load vma_list_exec/reload*/
    if (first == -1)
        return;

    struct stat s;
    for (; vma->vm_start != NULL; vma++) {
        add = _sgx_vma_alloc(sgxmm.in.prev, &sgxmm.in);
        _sgx_vma_fill(add, vma->vm_start, vma->vm_end-vma->vm_start, vma->perm, -1, vma->offset);
        if (vma->comment[0] != '\0') {
            if (vma->comment[0] != '[') {
                int fd;

                fd = dynamorio_syscall(SYS_stat, 2, vma->comment, &s);
                YPHASSERT(fd == 0);

                add->dev = s.st_dev;
                add->inode = s.st_ino;
            }
            strncpy(add->comment, vma->comment, 80);
        }
    }
}


/* exported for debugging */
sgx_mm_t* sgx_mm_getmm(void)
{
    return &sgxmm;
}


sgx_vm_area_t* _sgx_vma_alloc(list_t* llp, list_t* lln)
{
    YPHASSERT(llp != NULL && lln != NULL);

    sgx_vm_area_t* vma = NULL;
    list_t* ll = NULL;

    ll = sgxmm.un;
    sgxmm.un = sgxmm.un->next;
    sgxmm.nin ++, sgxmm.nun --;

    vma = list_entry(ll, sgx_vm_area_t, ll);
    YPHASSERT(vma != NULL);

    list_add(llp, lln, ll);

    return vma;
}

void _sgx_vma_free(sgx_vm_area_t* vma)
{
    YPHASSERT(vma != NULL);

    list_t* ll = &vma->ll;
    list_del(ll);

    ll->prev = NULL; /* set prev to NULL if not used */
    ll->next = sgxmm.un;
    sgxmm.un = ll;
    sgxmm.nin --, sgxmm.nun ++;
}


/* fill a SGX-vma to track the mmap event */
static void _sgx_vma_fill(sgx_vm_area_t* vma, byte* vm_start, size_t len, ulong prot, int fd, ulong offs)
{
    YPHASSERT(vma != NULL);

    vma->vm_start = vm_start;
    vma->vm_end = vm_start + len;
    vma->vm_sgx = sgx_mm_ext2itn(vm_start);

    vma->offset = offs;
    vma->perm = prot;


    struct stat s;
    int res;

    if (fd == -1) {
        vma->dev = 0;
        vma->inode = 0;
        *(long*)vma->comment = 0;
    }
    else {
        res = dynamorio_syscall(SYS_fstat, 2, fd, &s);
        YPHASSERT(res == 0);

        vma->dev = s.st_dev;
        vma->inode = s.st_ino;
        sgx_vma_get_cmt(fd, vma->comment);
    }
}

/* deep copy except the ll field */
static void _sgx_vma_deep_copy(sgx_vm_area_t* dst, sgx_vm_area_t* src)
{
    // don't use :*dst = *src;
    dst->vm_start =  src->vm_start;
    dst->vm_end =  src->vm_end;
    dst->vm_sgx =  src->vm_sgx;
    dst->perm =  src->perm;
    dst->dev =  src->dev;
    dst->inode =  src->inode;
    dst->offset =  src->offset;

    strncpy(dst->comment, src->comment, SGX_VMA_CMT_LEN);
}


/* Test if current VMA is coverred by a given region */
static vma_overlap_t _sgx_vma_overlap(byte* vma_start, byte* vma_end, byte* ref_start, byte* ref_end)
{
    if (ref_start < vma_start) {
        if (ref_end <= vma_start)
            return OVERLAP_NONE;
        else if (ref_end < vma_end)
            return OVERLAP_HEAD;
        else
            return OVERLAP_SUP;
    }
    else if (ref_start == vma_start) {
        if (ref_end < vma_end)
            return OVERLAP_HEAD;
        else
            return OVERLAP_SUP;
    }
    else if (ref_start < vma_end) {
        if (ref_end < vma_end)
            return OVERLAP_SUB;
        else
            return OVERLAP_TAIL;
    }
    else {
        return OVERLAP_NONE;
    }
}


/* Merge adjacent vmas */
static sgx_vm_area_t* _sgx_vma_merge(sgx_vm_area_t* vma)
{
    sgx_vm_area_t* prev;
    sgx_vm_area_t* next;
    list_t* llp;
    list_t* lln;
    list_t* ll;


    YPHASSERT(vma != NULL && &vma->ll != &sgxmm.in);
    ll = &vma->ll;
    llp = ll->prev;
    lln = ll->next;

    if (llp != &sgxmm.in) {
        /* adjacent? */
        prev = list_entry(llp, sgx_vm_area_t, ll);

        if (prev->vm_end == vma->vm_start &&    /* free-prev vma */
                prev->perm == vma->perm &&
                prev->dev == vma->dev &&
                prev->inode == vma->inode &&
                *(long*)prev->comment == *(long*)(vma->comment) &&
                (prev->offset == vma->offset || prev->offset + (prev->vm_end - prev->vm_start) == vma->offset )) {

            vma->vm_start = prev->vm_start;
            if (vma->inode != 0)
                vma->offset = prev->offset;

            _sgx_vma_free(prev);
        }
    }

    if (lln != &sgxmm.in) {
        /* adjacent? */
        next = list_entry(lln, sgx_vm_area_t, ll);

        if (vma->vm_end == next->vm_start &&    /* vma free-next */
                vma->perm == next->perm &&
                vma->dev == next->dev &&
                vma->inode == next->inode &&
                *(long*)vma->comment == *(long*)(next->comment) &&
                (vma->offset == next->offset || vma->offset + (vma->vm_end - vma->vm_start) == next->offset )) {

            vma->vm_end = next->vm_end;

            _sgx_vma_free(next);
        }
    }

    return vma;
}


/* return the number of bytes overlapped */
static size_t _sgx_vma_split(vma_overlap_t ot, sgx_vm_area_t** head, sgx_vm_area_t *vma, sgx_vm_area_t** tail, byte* ref_start, byte *ref_end)
{
    sgx_vm_area_t *add = NULL;
    list_t *ll = &vma->ll;
    list_t *llp = ll->prev;
    list_t *lln = ll->next;
    size_t len = 0;

    YPHASSERT(vma != NULL);
    *head = NULL;
    *tail = NULL;
    switch (ot) {
        case OVERLAP_NONE:

            break;
        case OVERLAP_HEAD:  /* head vma  NULL */
            add = _sgx_vma_alloc(llp, ll);

            _sgx_vma_deep_copy(add, vma);
            add->vm_end = ref_end;
            vma->vm_start = ref_end;
            len = add->vm_end - add->vm_start;
            if (vma->inode != 0)
                vma->offset += len;

            *head = add;

            break;
        case OVERLAP_TAIL:  /* NULL vma tail */
            add = _sgx_vma_alloc(ll, lln);

            _sgx_vma_deep_copy(add, vma);
            vma->vm_end = ref_start;
            add->vm_start = ref_start;
            len = add->vm_end - add->vm_start;
            if (add->inode != 0)
                add->offset += len;

            *tail = add;

            break;
        case OVERLAP_SUP:   /* NULL vma NULL */
            len = vma->vm_end - vma->vm_start;

            break;
        case OVERLAP_SUB:   /* head vma tail */
            if (vma->vm_start != ref_start) {
                add = _sgx_vma_alloc(llp, ll);

                _sgx_vma_deep_copy(add, vma);
                add->vm_end = ref_start;
                vma->vm_start = ref_start;
                if (vma->inode != 0)
                    vma->offset += add->vm_end - add->vm_start;

                *head = add;
            }

            if (vma->vm_end != ref_end) {
                add = _sgx_vma_alloc(ll, lln);

                _sgx_vma_deep_copy(add, vma);
                vma->vm_end = ref_end;
                add->vm_start = ref_end;
                if (add->inode != 0)
                    add->offset += vma->vm_end - vma->vm_start;

                *tail = add;
            }
            len = ref_end - ref_start;

            break;
        default:
            YPHASSERT(false);

            break;
    } /* end switch */

    return len;
}



/* Allocate a SGX-vma to track the mmap event */
int sgx_mm_mprotect(byte* ext_addr, size_t len, uint prot)
{
    // YPHASSERT (!sgx_mm_within(ext_addr, len));

    sgx_vm_area_t *vma = NULL;
    sgx_vm_area_t *head = NULL;
    sgx_vm_area_t *tail = NULL;
    list_t *ll = sgxmm.in.next;
    vma_overlap_t ot = OVERLAP_NONE;
    byte *ref_start = ext_addr;
    byte *ref_end = ext_addr + len;
    bool ctuw = true;

    len = (len + SGX_PAGE_SIZE - 1) & ~(SGX_PAGE_SIZE - 1);
    YPHASSERT(ll != &sgxmm.in);
    /* There is a corner case that, by adding a new region, two previous seperated continus regions are now merged into a single one. */
    /* We don't consider this case for simplicity */
    while (ctuw && ll != &sgxmm.in) {
        vma = list_entry(ll, sgx_vm_area_t, ll);
        ot = _sgx_vma_overlap(vma->vm_start, vma->vm_end, ref_start, ref_end);

        switch (ot) {
            case OVERLAP_NONE:
                if (vma->vm_end <= ref_start)    /* no need to check anymore */
                    ll = ll->next;
                else
                    ctuw = false;

                break;

            case OVERLAP_HEAD:
                if (vma->perm == prot)  {/* already have the same property */
                    len -= vma->vm_end - vma->vm_start;
                }
                else {
                    len -= _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);
                    YPHASSERT(head != NULL);

                    head->perm = prot;
                    _sgx_vma_merge(head);
                }

                ctuw = false;
                break;

            case OVERLAP_TAIL:
                if (vma->perm == prot) { /* already have the same property */
                    len -= vma->vm_start - vma->vm_end;
                    ll = ll->next;
                }
                else {
                    len -= _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);
                    YPHASSERT(tail != NULL);

                    tail->perm = prot;
                    /* please don't call _sgx_vma_merge */;

                    ll = tail->ll.next;
                }

                break;

            case OVERLAP_SUP:
                if (vma->perm == prot) { /* already have the same property */
                }
                else {
                    vma->perm = prot;
                }
                len -= vma->vm_end - vma->vm_start;
                _sgx_vma_merge(vma);

                ll = ll->next;

                break;

            case OVERLAP_SUB:
                if (vma->perm == prot) { /* already have the same property */
                }
                else {
                    len -= _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);
                    vma->perm = prot;
                }

                /* Please don't invoke _sgx_vma_merge */
                vma->perm = prot;
                len = 0;

                ctuw = false;
                break;

            default:
                YPHASSERT(false);
                break;
        } /* end switch */
    }/* end while */


    if (len != 0)
        return -1;
    else
        return 0;
}


/* track munmap event */
sgx_vm_area_t* sgx_mm_munmap(byte* ext_addr, size_t len)
{
    // YPHASSERT (!sgx_mm_within(ext_addr, len));
    sgx_vm_area_t *vma = NULL;
    sgx_vm_area_t *head = NULL;
    sgx_vm_area_t *tail = NULL;
    byte *ref_start = ext_addr;
    byte *ref_end = ext_addr + len;

    list_t *ll = sgxmm.in.next;
    vma_overlap_t ot;
    bool ctuw = true;

    len = (len + SGX_PAGE_SIZE - 1) & ~(SGX_PAGE_SIZE - 1);
    while (ctuw && ll != &sgxmm.in) {
        vma = list_entry(ll, sgx_vm_area_t, ll);
        ot = _sgx_vma_overlap(vma->vm_start, vma->vm_end, ext_addr, ext_addr + len);

        switch (ot) {
            case OVERLAP_NONE:
                if (vma->vm_start < ref_start) {
                    ll = ll->next;
                }
                else {
                    ctuw = false;
                }

                break;
            case OVERLAP_HEAD:
                _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);

                YPHASSERT(head != NULL);
                _sgx_vma_free(head);
                ctuw = false;

                break;
            case OVERLAP_TAIL:
                _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);

                YPHASSERT(tail != NULL);
                _sgx_vma_free(tail);

                ll = vma->ll.next;

                break;
            case OVERLAP_SUP:
                ll = vma->ll.next;

                _sgx_vma_free(vma);

                break;
            case OVERLAP_SUB:
                _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);

                YPHASSERT(vma != NULL);
                _sgx_vma_free(vma);
                ctuw = false;

                break;
            default:
                YPHASSERT(false);
                break;
        } /* end switch */
    }/* end while */

    return NULL;
}


/* track mmap event */
sgx_vm_area_t* sgx_mm_mmap(byte* ext_addr, size_t len, ulong prot, ulong flags, ulong ufd, ulong offs)
{
    sgx_vm_area_t* vma = NULL;
    int fd = (int)ufd;

    len = (len + SGX_PAGE_SIZE - 1) & ~(SGX_PAGE_SIZE - 1);

    /* unmap the overllapped area first */
    sgx_mm_munmap(ext_addr, len);

    /* find the location to insert vma */
    list_t *ll = sgxmm.in.next;
    while (ll != &sgxmm.in) {
        vma = list_entry(ll, sgx_vm_area_t, ll);
        if (vma->vm_start < ext_addr)
            ll = ll->next;
        else
            break;
    }


    /* create a new vma*/
    vma = _sgx_vma_alloc(ll->prev, ll);
    _sgx_vma_fill(vma, ext_addr, len, prot, fd, offs);

    /* do merge if needs for anonymous-mappings */
    if (fd == -1)
        vma = _sgx_vma_merge(vma);

    return vma;
}

/* allocate vma for the first segment, it's usually .text */
sgx_vm_area_t* ___sgx_vma_alloc_text(uint npgs)
{
    sgx_vm_area_t *vma;
    sgx_vm_area_t *add;
    list_t *llp;

    add = list_entry(sgxmm.un, sgx_vm_area_t, ll);
    sgxmm.un = sgxmm.un->next;

    llp = sgxmm.in.prev;
    if (llp == &sgxmm.in) {
        YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE >= npgs);
        add->vm_sgx = 0;
        /*//add->npgs = npgs;*/

        sgxmm.in.next = &add->ll;
        sgxmm.in.prev = &add->ll;
        add->ll.prev = &sgxmm.in;
        add->ll.next = &sgxmm.in;

        return add;
    }
    else {
        vma = list_entry(llp, sgx_vm_area_t, ll);

        /*add->vm_sgx = vma->vm_sgx + vma->npgs;*/
        //add->npgs = npgs;

        list_add(llp, &sgxmm.in, &add->ll);
        return add;
    }
}

sgx_vm_area_t* ___sgx_vma_alloc_data(uint npgs)
{
    YPHASSERT(sgxmm.in.next != &sgxmm.in);
    return ___sgx_vma_alloc_text(npgs);
}

sgx_vm_area_t* ___sgx_vma_alloc_bss(uint npgs)
{
    return ___sgx_vma_alloc_data(npgs);
}



sgx_vm_area_t* ___sgx_vma_alloc_anon(uint npgs)
{
    sgx_vm_area_t *prev;
    sgx_vm_area_t *next;
    sgx_vm_area_t *add;
    list_t *llp;
    list_t *lln;

    llp = sgxmm.in.next;
    lln = llp->next;

    add = list_entry(sgxmm.un, sgx_vm_area_t, ll);
    sgxmm.un = sgxmm.un->next;

    if (llp == &sgxmm.in) {
        YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE >= npgs);
        add->vm_sgx = 0;
        //add->npgs = npgs;

        sgxmm.in.next = &add->ll;
        sgxmm.in.prev = &add->ll;
        add->ll.prev = &sgxmm.in;
        add->ll.next = &sgxmm.in;

        return add;
    }
    else if (lln == &sgxmm.in) {
        prev = list_entry(llp, sgx_vm_area_t, ll);
        if (false) {//prev->vm_sgx >= npgs) {
            add->vm_sgx = 0;
            //add->npgs = npgs;

            list_add(&sgxmm.in, llp, &add->ll);
        }
        else {
            /*YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE > prev->vm_sgx + prev->npgs);*/
            //add->vm_sgx = prev->vm_sgx + prev->npgs;
            //add->npgs = npgs;

            list_add(llp, &sgxmm.in, &add->ll);
        }
        return add;
    }
    else {
        while (lln != &sgxmm.in) {
            uint gap = 0;

            prev = list_entry(llp, sgx_vm_area_t, ll);
            next = list_entry(lln, sgx_vm_area_t, ll);

            /* adjcent? */
            /*gap = next->vm_sgx - (prev->vm_sgx + prev->npgs);*/
            if (gap >= npgs) {
                //add->vm_sgx = prev->vm_sgx + prev->npgs;
                //add->npgs = npgs;

                list_add(llp, lln, &add->ll);
                return add;
            }
            else {
                llp = lln;
                lln = lln->next;
            }
        }
        if (lln == &sgxmm.in) {
            prev = list_entry(llp, sgx_vm_area_t, ll);
            /*YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE > prev->vm_sgx + prev->npgs);*/
            //add->vm_sgx = prev->vm_sgx + prev->npgs;
            //add->npgs = npgs;

            list_add(llp, &sgxmm.in, &add->ll);
            return add;
        }
    }

    /* Failed: free add and return NULL */
    add->ll.next = sgxmm.un;
    sgxmm.un = &add->ll;

    return NULL;
}


sgx_vm_area_t* __sgx_vma_alloc(sgx_vma_type t, uint npgs)
{
    switch(t) {
        case SGX_VMA_TEXT:
            return ___sgx_vma_alloc_text(npgs);
        case SGX_VMA_DATA:
            return ___sgx_vma_alloc_data(npgs);
        case SGX_VMA_BSS:
            return ___sgx_vma_alloc_bss(npgs);
        case SGX_VMA_ANON:
            return ___sgx_vma_alloc_anon(npgs);
            //return ___sgx_vma_alloc_bss(npgs);
        default:
            return NULL;
    }
}


void* __sgx_vma_free(uint first_page, uint npgs)
{
    sgx_vm_area_t *vma = NULL;
    list_t *llp = NULL;
    list_t *ll = NULL;
    void * ext_addr = NULL;

    llp = sgxmm.in.next;
    if (llp == &sgxmm.in)
        return NULL;

    if (llp->next == &sgxmm.in) {
        vma = list_entry(llp, sgx_vm_area_t, ll);

        // if (vma->vm_sgx == first_page && vma->npgs == npgs) {
        //     ext_addr = vma->vm_start;

        //     sgxmm.in.next = &sgxmm.in;
        //     sgxmm.in.prev = &sgxmm.in;

        //     llp->next = sgxmm.un;
        //     sgxmm.un = llp;
        // }

        return ext_addr;
    }

    for (ll = llp->next; ll != &sgxmm.in; ll = ll->next) {
        vma = list_entry(ll, sgx_vm_area_t, ll);

        /*if (vma->vm_sgx == first_page && vma->npgs == npgs) {*/
            /*ext_addr = vma->vm_start;*/
            /*list_del(ll);*/

            /*ll->next = sgxmm.un;*/
            /*sgxmm.un = ll;*/
            /*break;*/
        /*}*/
    }
    return ext_addr;
}


// byte* sgx_mm_mmap(byte *exp_addr, byte *ext_addr, size_t len, ulong prot, ulong flags, ulong fd, ulong offs)
// {
//     sgx_vm_area_t* vma;
//     sgx_vma_type t;
//     uint npgs;

//     npgs = (len + SGX_PAGE_SIZE - 1) / SGX_PAGE_SIZE;
//     if (exp_addr == NULL) {
//         if (fd == (ulong)-1)
//             t = SGX_VMA_ANON;
//         else
//             t = SGX_VMA_TEXT;
//     }
//     else {
//         YPHASSERT(ext_addr != NULL);
//         if (fd == (ulong)-1)
//             t = SGX_VMA_BSS;
//         else
//             t = SGX_VMA_DATA;
//     }
//     vma = __sgx_vma_alloc(t, npgs);
//     if (vma == NULL)
//         return NULL;

//     switch (t) {
//         case SGX_VMA_ANON:
//             vma->vm_start = NULL;
//             break;

//         case SGX_VMA_TEXT:
//             vma->vm_start = ext_addr;
//             break;

//         case SGX_VMA_DATA:
//             vma->vm_start = ext_addr;
//             break;

//         case SGX_VMA_BSS:
//             vma->vm_start = NULL;
//             break;

//         default:
//             return NULL;
//     }

//     /* fill in other content */
//     vma->perm = prot;
//     vma->offset = offs;
//     /* anonymous mmap ? */
//     if (fd == (ulong)-1)
//         vma->inode = 0;
//     else
//         vma->inode = 0; /* fourcely set it to 0 */

//     sgx_vma_get_cmt(fd, vma->comment);

//     return (sgxmm.vm_base + vma->vm_sgx * SGX_PAGE_SIZE);
// }

// void* sgx_mm_munmap(byte* itn_addr, size_t len)
// {
//     void *ext_addr;
//     uint pgidx;
//     uint npgs;

//     YPHASSERT(itn_addr > sgxmm.vm_base);
//     pgidx = (itn_addr - sgxmm.vm_base) / SGX_PAGE_SIZE;
//     npgs = (len + SGX_PAGE_SIZE - 1) / SGX_PAGE_SIZE;
//     ext_addr = __sgx_vma_free(pgidx, npgs);

//     YPHASSERT(ext_addr != NULL);

//     return ext_addr;
// }


// void sgx_mm_mprotect(byte* start, byte* end)
// {

// }

byte* sgx_mm_databss_itn2ext(byte* itn_addr, ulong fd)
{
    //uint pgidx = (itn_addr - sgxmm.vm_base) / SGX_PAGE_SIZE;
    sgx_vm_area_t *vma = NULL;
    list_t *ll = NULL;

    for (ll = sgxmm.in.next; ll != &sgxmm.in; ll = ll->next) {
        YPHASSERT(ll != &sgxmm.in);
        vma = list_entry(ll, sgx_vm_area_t, ll);

        //if (vma->vm_sgx + vma->npgs == pgidx) {
            //YPHASSERT(vma->fd == fd);
            /*return vma->vm_start + vma->npgs * SGX_PAGE_SIZE;*/
        {}
    }
    return NULL;
}


sgx_vm_area_t* sgx_procmaps_mmap(byte* addr)
{
    // sgx_vm_area_t *vma;
    // sgx_vm_area_t *add;
    // list_t *llp;

    // int npgs;
    // add = list_entry(sgxmm.un, sgx_vm_area_t, ll);
    // sgxmm.un = sgxmm.un->next;

    // llp = sgxmm.in.prev;
    // if (llp == &sgxmm.in) {
    //     YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE >= npgs);
    //     add->vm_sgx = 0;
    //     //add->npgs = npgs;

    //     sgxmm.in.next = &add->ll;
    //     sgxmm.in.prev = &add->ll;
    //     add->ll.prev = &sgxmm.in;
    //     add->ll.next = &sgxmm.in;

    //     return add;
    // }
    // else {
    //     vma = list_entry(llp, sgx_vm_area_t, ll);
    //     YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE > vma->vm_sgx + vma->npgs + npgs);

    //     add->vm_sgx = vma->vm_sgx + vma->npgs;
    //     //add->npgs = npgs;

    //     list_add(llp, &sgxmm.in, &add->ll);
    //     return add;
    return NULL;

}
