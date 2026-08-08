// bootloader/efi_main.c
// UEFI Bootloader para Aurora OS

#include <efi.h>
#include <efilib.h>
#include <stdint.h>

#define KERNEL_PATH L"\\kernel.elf"
#define ET_EXEC 2

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;

#define EI_NIDENT 16
#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define EM_X86_64 62
#define PT_LOAD   1

struct Elf64_Ehdr {
    unsigned char e_ident[EI_NIDENT]; Elf64_Half e_type; Elf64_Half e_machine; Elf64_Word e_version;
    Elf64_Addr e_entry; Elf64_Off e_phoff; Elf64_Off e_shoff; Elf64_Word e_flags;
    Elf64_Half e_ehsize; Elf64_Half e_phentsize; Elf64_Half e_phnum; Elf64_Half e_shentsize;
    Elf64_Half e_shnum; Elf64_Half e_shstrndx;
};
struct Elf64_Phdr {
    Elf64_Word p_type; Elf64_Word p_flags; Elf64_Off p_offset; Elf64_Addr p_vaddr; Elf64_Addr p_paddr;
    Elf64_Xword p_filesz; Elf64_Xword p_memsz; Elf64_Xword p_align;
};
struct kernel_boot_info {
    uint64_t fb_base, fb_size; uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    uint64_t memmap, memmap_size, memmap_desc_size; uint32_t memmap_desc_ver;
};
typedef struct { UINTN map_size, map_key, desc_size; UINT32 desc_version; EFI_MEMORY_DESCRIPTOR *map; } mem_map_t;
static mem_map_t mem_map = {0};
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
static EFI_LOADED_IMAGE *loaded_image = NULL;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;

static EFI_STATUS get_memory_map(EFI_HANDLE image_handle) {
    EFI_STATUS status;
    mem_map.map_size = 0;
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &mem_map.map_size, NULL, &mem_map.map_key, &mem_map.desc_size, &mem_map.desc_version);
    if (status != EFI_BUFFER_TOO_SMALL) return status;
    mem_map.map_size += 4 * mem_map.desc_size;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, mem_map.map_size, (VOID**)&mem_map.map);
    if (EFI_ERROR(status)) return status;
    return uefi_call_wrapper(BS->GetMemoryMap, 5, &mem_map.map_size, mem_map.map, &mem_map.map_key, &mem_map.desc_size, &mem_map.desc_version);
}

static EFI_STATUS map_page_4k(UINT64 *pml4, EFI_PHYSICAL_ADDRESS virt, EFI_PHYSICAL_ADDRESS phys) {
    UINTN pml4_idx = (virt >> 39) & 0x1FF, pdpt_idx = (virt >> 30) & 0x1FF;
    UINTN pd_idx = (virt >> 21) & 0x1FF, pt_idx = (virt >> 12) & 0x1FF;
    if (!(pml4[pml4_idx] & 1)) {
        EFI_PHYSICAL_ADDRESS a; EFI_STATUS s = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, 1, &a);
        if (EFI_ERROR(s)) return s; ZeroMem((VOID*)a, 4096); pml4[pml4_idx] = a | 3;
    }
    UINT64 *pdpt = (UINT64*)(pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & 1)) {
        EFI_PHYSICAL_ADDRESS a; EFI_STATUS s = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, 1, &a);
        if (EFI_ERROR(s)) return s; ZeroMem((VOID*)a, 4096); pdpt[pdpt_idx] = a | 3;
    }
    UINT64 *pd = (UINT64*)(pdpt[pdpt_idx] & ~0xFFFULL);
    if (!(pd[pd_idx] & 1)) {
        EFI_PHYSICAL_ADDRESS a; EFI_STATUS s = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, 1, &a);
        if (EFI_ERROR(s)) return s; ZeroMem((VOID*)a, 4096); pd[pd_idx] = a | 3;
    }
    UINT64 *pt = (UINT64*)(pd[pd_idx] & ~0xFFFULL);
    pt[pt_idx] = phys | 3;
    return EFI_SUCCESS;
}

static EFI_STATUS load_kernel(EFI_FILE *root, VOID **entry_point, UINT64 *pml4) {
    EFI_FILE *file = NULL;
    EFI_STATUS status = uefi_call_wrapper(root->Open, 5, root, &file, KERNEL_PATH, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) { Print(L"[BOOT] Error abriendo kernel.elf: %r\n", status); return status; }
    struct Elf64_Ehdr ehdr; UINTN size = sizeof(ehdr);
    status = uefi_call_wrapper(file->Read, 3, file, &size, &ehdr);
    if (EFI_ERROR(status) || size != sizeof(ehdr)) goto cleanup;
    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 || ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) { Print(L"[BOOT] No es un ELF valido\n"); status = EFI_INVALID_PARAMETER; goto cleanup; }
    if (ehdr.e_machine != EM_X86_64) { Print(L"[BOOT] ELF no es x86_64 (machine=%d)\n", ehdr.e_machine); status = EFI_UNSUPPORTED; goto cleanup; }
    if (ehdr.e_type != ET_EXEC) { Print(L"[BOOT] ELF no es ET_EXEC (type=%d)\n", ehdr.e_type); status = EFI_UNSUPPORTED; goto cleanup; }

    // PT_LOAD pueden compartir paginas virtuales. Cargamos todo el rango en un unico bloque fisico.
    UINT64 image_min = UINT64_MAX, image_max = 0; UINTN load_count = 0;
    for (UINTN i = 0; i < ehdr.e_phnum; i++) {
        struct Elf64_Phdr phdr; UINTN n = sizeof(phdr);
        status = uefi_call_wrapper(file->SetPosition, 2, file, ehdr.e_phoff + i * ehdr.e_phentsize); if (EFI_ERROR(status)) goto cleanup;
        status = uefi_call_wrapper(file->Read, 3, file, &n, &phdr); if (EFI_ERROR(status) || n != sizeof(phdr)) goto cleanup;
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;
        UINT64 start = phdr.p_vaddr & ~0xFFFULL, end = phdr.p_vaddr + phdr.p_memsz;
        if (end < phdr.p_vaddr || end > UINT64_MAX - 0xFFFULL) { status = EFI_LOAD_ERROR; goto cleanup; }
        end = (end + 0xFFFULL) & ~0xFFFULL;
        if (start < image_min) image_min = start; if (end > image_max) image_max = end; load_count++;
    }
    if (load_count == 0 || image_max <= image_min) { Print(L"[BOOT] No hay segmentos PT_LOAD validos\n"); status = EFI_LOAD_ERROR; goto cleanup; }
    UINT64 image_size = image_max - image_min; UINTN image_pages = (UINTN)(image_size / 0x1000ULL); EFI_PHYSICAL_ADDRESS image_phys = 0;
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, image_pages, &image_phys);
    if (EFI_ERROR(status)) { Print(L"[BOOT] No se pudo reservar la imagen del kernel (%d paginas)\n", image_pages); goto cleanup; }
    ZeroMem((VOID*)image_phys, image_size);
    Print(L"[BOOT] Kernel image: vaddr=0x%lx size=0x%lx paddr=0x%lx pages=%d\n", image_min, image_size, image_phys, image_pages);

    EFI_PHYSICAL_ADDRESS entry_phys = 0;
    for (UINTN i = 0; i < ehdr.e_phnum; i++) {
        struct Elf64_Phdr phdr; UINTN n = sizeof(phdr);
        status = uefi_call_wrapper(file->SetPosition, 2, file, ehdr.e_phoff + i * ehdr.e_phentsize); if (EFI_ERROR(status)) goto cleanup;
        status = uefi_call_wrapper(file->Read, 3, file, &n, &phdr); if (EFI_ERROR(status) || n != sizeof(phdr)) goto cleanup;
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;
        UINT64 offset = phdr.p_vaddr - image_min; EFI_PHYSICAL_ADDRESS paddr = image_phys + offset;
        status = uefi_call_wrapper(file->SetPosition, 2, file, phdr.p_offset); if (EFI_ERROR(status)) goto cleanup;
        UINTN read_size = (UINTN)phdr.p_filesz;
        status = uefi_call_wrapper(file->Read, 3, file, &read_size, (VOID*)paddr); if (EFI_ERROR(status) || read_size != (UINTN)phdr.p_filesz) goto cleanup;
        if (phdr.p_memsz > phdr.p_filesz) ZeroMem((VOID*)(paddr + phdr.p_filesz), phdr.p_memsz - phdr.p_filesz);
        UINT64 map_start = phdr.p_vaddr & ~0xFFFULL, map_end = (phdr.p_vaddr + phdr.p_memsz + 0xFFFULL) & ~0xFFFULL;
        for (UINT64 v = map_start; v < map_end; v += 0x1000ULL) {
            status = map_page_4k(pml4, v, image_phys + (v - image_min)); if (EFI_ERROR(status)) goto cleanup;
        }
        if (ehdr.e_entry >= phdr.p_vaddr && ehdr.e_entry < phdr.p_vaddr + phdr.p_filesz) entry_phys = image_phys + (ehdr.e_entry - image_min);
        Print(L"[BOOT] Segmento %d: vaddr=0x%lx paddr=0x%lx pages=%d\n", i, phdr.p_vaddr, paddr, (UINTN)((map_end - map_start) / 0x1000ULL));
    }
    if (!entry_phys) { Print(L"[BOOT] ERROR: Entry point no pertenece a ningun PT_LOAD\n"); status = EFI_LOAD_ERROR; goto cleanup; }
    *entry_point = (VOID*)ehdr.e_entry;
    Print(L"[BOOT] Entry point: 0x%lx\n", ehdr.e_entry);
    Print(L"[BOOT] Entry physical: 0x%lx bytes:", entry_phys);
    for (UINTN i = 0; i < 32; i++) Print(L" %02x", ((UINT8*)entry_phys)[i]);
    Print(L"\n");
    status = EFI_SUCCESS;
cleanup:
    uefi_call_wrapper(file->Close, 1, file); return status;
}

static EFI_STATUS setup_framebuffer(void) {
    EFI_STATUS status = uefi_call_wrapper(BS->LocateProtocol, 3, &gEfiGraphicsOutputProtocolGuid, NULL, (VOID**)&gop);
    if (EFI_ERROR(status) || !gop) { Print(L"[BOOT] GOP no disponible\n"); return status; }
    Print(L"[BOOT] FB: %dx%d @ 0x%lx size=0x%lx pitch=%d\n", gop->Mode->Info->HorizontalResolution, gop->Mode->Info->VerticalResolution, gop->Mode->FrameBufferBase, gop->Mode->FrameBufferSize, gop->Mode->Info->PixelsPerScanLine * 4); return EFI_SUCCESS;
}

static EFI_STATUS build_page_tables(EFI_PHYSICAL_ADDRESS *pml4_out) {
    EFI_STATUS status; EFI_PHYSICAL_ADDRESS pml4_addr = 0, pdpt_addr = 0; EFI_PHYSICAL_ADDRESS pd_addrs[4] = {0};
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, 1, &pml4_addr); if (EFI_ERROR(status)) return status;
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, 1, &pdpt_addr); if (EFI_ERROR(status)) return status;
    ZeroMem((VOID*)pml4_addr, 4096); ZeroMem((VOID*)pdpt_addr, 4096); UINT64 *pml4 = (UINT64*)pml4_addr; UINT64 *pdpt = (UINT64*)pdpt_addr; pml4[0] = pdpt_addr | 3;
    for (int i = 0; i < 4; i++) {
        status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, 1, &pd_addrs[i]); if (EFI_ERROR(status)) return status;
        ZeroMem((VOID*)pd_addrs[i], 4096); pdpt[i] = pd_addrs[i] | 3; UINT64 *pd = (UINT64*)pd_addrs[i];
        for (int j = 0; j < 512; j++) pd[j] = ((UINT64)i * 512 + j) * 0x200000ULL | 0x83;
    }
    pml4[511] = pdpt_addr | 3; *pml4_out = pml4_addr; return EFI_SUCCESS;
}

static EFI_STATUS copy_memory_map(VOID **map_out, UINTN *map_size_out, UINTN *desc_size_out, UINT32 *desc_ver_out) {
    UINTN size = mem_map.map_size; EFI_STATUS status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, size, map_out); if (EFI_ERROR(status)) return status;
    CopyMem(*map_out, mem_map.map, size); *map_size_out = size; *desc_size_out = mem_map.desc_size; *desc_ver_out = mem_map.desc_version; return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    InitializeLib(image_handle, system_table); Print(L"\n=== AURORA OS BOOTLOADER ===\n"); EFI_STATUS status;
    status = uefi_call_wrapper(BS->HandleProtocol, 3, image_handle, &LoadedImageProtocol, (VOID**)&loaded_image); if (EFI_ERROR(status)) return status;
    status = uefi_call_wrapper(BS->HandleProtocol, 3, loaded_image->DeviceHandle, &FileSystemProtocol, (VOID**)&fs); if (EFI_ERROR(status)) return status;
    EFI_FILE *root = NULL; status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root); if (EFI_ERROR(status)) return status;
    setup_framebuffer();
    EFI_PHYSICAL_ADDRESS pml4_addr = 0; status = build_page_tables(&pml4_addr); if (EFI_ERROR(status)) { Print(L"[BOOT] Error construyendo tablas de pagina: %r\n", status); return status; }
    VOID *entry_point = NULL; status = load_kernel(root, &entry_point, (UINT64*)pml4_addr); if (EFI_ERROR(status)) return status;
    VOID *map_copy = NULL; UINTN map_size = 0, desc_size = 0; UINT32 desc_ver = 0;
    status = copy_memory_map(&map_copy, &map_size, &desc_size, &desc_ver); if (EFI_ERROR(status)) { Print(L"[BOOT] Error copiando memmap: %r\n", status); return status; }
    struct kernel_boot_info *boot_info = NULL; status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, sizeof(*boot_info), (VOID**)&boot_info); if (EFI_ERROR(status)) return status;
    boot_info->fb_base = gop ? gop->Mode->FrameBufferBase : 0; boot_info->fb_size = gop ? gop->Mode->FrameBufferSize : 0; boot_info->fb_width = gop ? gop->Mode->Info->HorizontalResolution : 0; boot_info->fb_height = gop ? gop->Mode->Info->VerticalResolution : 0; boot_info->fb_pitch = gop ? gop->Mode->Info->PixelsPerScanLine * 4 : 0; boot_info->fb_bpp = 32;
    boot_info->memmap = (uint64_t)map_copy; boot_info->memmap_size = map_size; boot_info->memmap_desc_size = desc_size; boot_info->memmap_desc_ver = desc_ver;
    UINT8 *stack = NULL; status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, 16384, (VOID**)&stack); if (EFI_ERROR(status)) return status; UINT64 stack_top = ((UINT64)stack + 16384) & ~0xFULL;
    status = uefi_call_wrapper(BS->ExitBootServices, 2, image_handle, mem_map.map_key); if (EFI_ERROR(status)) { Print(L"[BOOT] ExitBootServices fallo: %r\n", status); return status; }
    __asm__ volatile("mov %0, %%cr3" : : "r"((UINT64)pml4_addr) : "memory"); __asm__ volatile("mov %0, %%rsp" : : "r"(stack_top) : "memory");
    typedef void (*kernel_entry_t)(struct kernel_boot_info*); kernel_entry_t entry = (kernel_entry_t)entry_point;
    Print(L"[BOOT] Tablas de pagina listas. Memmap copiado.\n"); Print(L"[BOOT] Saltando al kernel...\n"); entry(boot_info); return EFI_SUCCESS;
}
