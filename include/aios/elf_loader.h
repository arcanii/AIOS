#ifndef AIOS_ELF_LOADER_H
#define AIOS_ELF_LOADER_H

#include <stdint.h>

/*
 * AIOS ELF Loader
 *
 * Parses ELF64 headers and loads PT_LOAD segments into sandbox memory.
 * Supports two modes:
 *   1. AIOS binaries: linked at 0x21100000 (sandbox code base)
 *   2. Linux binaries: linked at arbitrary address, rebased to code base
 *
 * Used by the orchestrator when loading programs into sandbox slots.
 */

/* ELF64 types */
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;

/* ELF magic */
#define ELFMAG0  0x7f
#define ELFMAG1  'E'
#define ELFMAG2  'L'
#define ELFMAG3  'F'

/* ELF class */
#define ELFCLASS64  2

/* ELF data encoding */
#define ELFDATA2LSB 1  /* Little endian */

/* ELF machine types */
#define EM_AARCH64  183

/* ELF type */
#define ET_EXEC  2  /* Executable */
#define ET_DYN   3  /* Shared object (PIE) */

/* Program header types */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_NOTE    4
#define PT_GNU_STACK  0x6474e551

/* Program header flags */
#define PF_X  0x1
#define PF_W  0x2
#define PF_R  0x4

/* ELF64 file header */
typedef struct {
    uint8_t    e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff;      /* Program header table offset */
    Elf64_Off  e_shoff;      /* Section header table offset */
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;  /* Size of program header entry */
    Elf64_Half e_phnum;      /* Number of program headers */
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

/* ELF64 program header */
typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;    /* Offset in file */
    Elf64_Addr  p_vaddr;     /* Virtual address in memory */
    Elf64_Addr  p_paddr;     /* Physical address (usually same) */
    Elf64_Xword p_filesz;    /* Size in file */
    Elf64_Xword p_memsz;     /* Size in memory (>= filesz, diff is BSS) */
    Elf64_Xword p_align;
} Elf64_Phdr;

/* ── ELF detection ─────────────────────────────────── */

static inline int elf_is_valid(const uint8_t *data, uint32_t size) {
    if (size < sizeof(Elf64_Ehdr)) return 0;
    if (data[0] != ELFMAG0 || data[1] != ELFMAG1 ||
        data[2] != ELFMAG2 || data[3] != ELFMAG3) return 0;
    if (data[4] != ELFCLASS64) return 0;   /* Must be 64-bit */
    if (data[5] != ELFDATA2LSB) return 0;  /* Must be little-endian */

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    if (ehdr->e_machine != EM_AARCH64) return 0;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return 0;

    return 1;
}

/* Result of ELF loading */
typedef struct {
    uint64_t entry_point;    /* Virtual address of entry */
    uint64_t load_base;      /* Lowest vaddr of any LOAD segment */
    uint32_t total_memsz;    /* Total memory footprint */
    uint32_t total_filesz;   /* Total bytes copied from file */
    int      num_segments;   /* Number of LOAD segments processed */
    int      is_linux;       /* 1 if entry is NOT at sandbox code base */
} elf_load_result_t;

/* ── ELF loader ────────────────────────────────────── */

/*
 * Load an ELF file into sandbox memory.
 *
 * Parameters:
 *   file_data    - Complete ELF file contents (read from disk)
 *   file_size    - Size of file data in bytes
 *   code_dst     - Pointer to sandbox code region start
 *   code_base    - Virtual address of code region (e.g., 0x21100000)
 *   code_max     - Maximum code region size (e.g., 4 MiB)
 *   result       - Output: load results
 *
 * Returns: 0 on success, negative on error
 *   -1: not a valid ELF
 *   -2: wrong architecture
 *   -3: segment exceeds code region
 *   -4: no LOAD segments found
 */
static inline int elf_load(const uint8_t *file_data, uint32_t file_size,
                           volatile uint8_t *code_dst, uint64_t code_base,
                           uint32_t code_max, elf_load_result_t *result) {

    if (!elf_is_valid(file_data, file_size))
        return -1;

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file_data;

    /* Initialize result */
    result->entry_point = ehdr->e_entry;
    result->load_base = 0xFFFFFFFFFFFFFFFFULL;
    result->total_memsz = 0;
    result->total_filesz = 0;
    result->num_segments = 0;
    result->is_linux = 0;

    /* Find the lowest load address to calculate rebase offset */
    uint64_t lowest_vaddr = 0xFFFFFFFFFFFFFFFFULL;
    const uint8_t *phdr_base = file_data + ehdr->e_phoff;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(phdr_base + i * ehdr->e_phentsize);
        if (phdr->p_type == PT_LOAD && phdr->p_vaddr < lowest_vaddr) {
            lowest_vaddr = phdr->p_vaddr;
        }
    }

    if (lowest_vaddr == 0xFFFFFFFFFFFFFFFFULL)
        return -4;  /* No LOAD segments */

    result->load_base = lowest_vaddr;

    /* Determine if this is a native AIOS binary or a Linux binary */
    if (lowest_vaddr != code_base) {
        result->is_linux = 1;
    }

    /* Calculate rebase: map file's vaddr space to our code region
     * offset_in_code = vaddr - lowest_vaddr
     * This puts the first LOAD segment at code_dst[0] */
    uint64_t rebase = lowest_vaddr;

    /* Process each LOAD segment */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(phdr_base + i * ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD)
            continue;

        /* Calculate where in code region this segment goes */
        uint64_t offset_in_code = phdr->p_vaddr - rebase;

        /* Bounds check */
        if (offset_in_code + phdr->p_memsz > code_max)
            return -3;

        /* Check file offset bounds */
        if (phdr->p_offset + phdr->p_filesz > file_size)
            return -3;

        /* Copy file data into code region */
        const uint8_t *src = file_data + phdr->p_offset;
        volatile uint8_t *dst = code_dst + offset_in_code;

        for (uint64_t j = 0; j < phdr->p_filesz; j++) {
            dst[j] = src[j];
        }

        /* Zero BSS (memsz > filesz) */
        for (uint64_t j = phdr->p_filesz; j < phdr->p_memsz; j++) {
            dst[j] = 0;
        }

        result->total_filesz += (uint32_t)phdr->p_filesz;
        result->total_memsz += (uint32_t)phdr->p_memsz;
        result->num_segments++;
    }

    if (result->num_segments == 0)
        return -4;

    /* Adjust entry point for rebased binary */
    if (result->is_linux) {
        result->entry_point = code_base + (ehdr->e_entry - rebase);
    }

    return 0;
}

/* ── Raw binary loader (backward compat) ───────────── */

static inline int raw_load(const uint8_t *file_data, uint32_t file_size,
                           volatile uint8_t *code_dst, uint32_t code_max,
                           elf_load_result_t *result) {
    if (file_size > code_max)
        return -3;

    for (uint32_t i = 0; i < file_size; i++) {
        code_dst[i] = file_data[i];
    }

    result->entry_point = 0;  /* Use default entry (code base) */
    result->load_base = 0;
    result->total_filesz = file_size;
    result->total_memsz = file_size;
    result->num_segments = 1;
    result->is_linux = 0;

    return 0;
}

#endif /* AIOS_ELF_LOADER_H */
