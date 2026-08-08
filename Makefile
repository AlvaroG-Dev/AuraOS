# Makefile global - AuraOS

# Firmware OVMF is resolved from the project root.
OVMF_CODE = $(CURDIR)/OVMF_CODE.fd
OVMF_VARS = $(CURDIR)/OVMF_VARS.fd

.PHONY: all bootloader kernel image run run-debug clean sysroot

all: image

sysroot:
	@mkdir -p sysroot/system/icons sysroot/system/wallpapers
	@if [ ! -f sysroot/system/config.txt ]; then \
		echo "AuraOS v0.1.0 Initramfs Config" > sysroot/system/config.txt; \
	fi
	@if [ -d assets ]; then \
		echo "[Makefile] Copiando assets a sysroot..."; \
		cp -r assets/* sysroot/ 2>/dev/null || true; \
	fi
	tar --format=ustar -cf kernel/initrd.tar -C sysroot .

bootloader:
	$(MAKE) -C bootloader install

kernel: sysroot
	$(MAKE) -C kernel

image: bootloader kernel
	mkdir -p esp/EFI/BOOT
	cp bootloader/BOOTX64.EFI esp/EFI/BOOT/
	cp kernel/kernel.elf esp/
	dd if=/dev/zero of=aurora.img bs=1M count=64
	mkfs.fat -F 32 aurora.img
	mcopy -i aurora.img -s esp/EFI ::
	mcopy -i aurora.img -s kernel/kernel.elf ::

run: image
	@test -f "$(OVMF_VARS)" || (echo "ERROR: $(OVMF_VARS) no encontrado en la raiz del proyecto" && exit 1)
	@test -f "$(OVMF_CODE)" || (echo "ERROR: $(OVMF_CODE) no encontrado en la raiz del proyecto" && exit 1)
	@echo "[QEMU] OVMF_CODE=$(OVMF_CODE)"
	@echo "[QEMU] OVMF_VARS=$(OVMF_VARS)"
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-drive format=raw,file=aurora.img \
		-serial stdio \
		-m 512M \
		-cpu qemu64 \
		-display none \
		-no-reboot -no-shutdown

clean:
	$(MAKE) -C bootloader clean
	$(MAKE) -C kernel clean
	rm -rf sysroot kernel/initrd.tar aurora.img esp