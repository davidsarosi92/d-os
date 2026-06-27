CC      := gcc
AS      := nasm
LD      := ld

INCLUDES := -Ikernel/includes
CFLAGS   := -m32 -ffreestanding -fno-stack-protector -fno-pie -nostdlib \
            -Wall -Wextra -Wno-unused-parameter -std=c11 $(INCLUDES)
ASFLAGS  := -f elf32
LDFLAGS  := -m elf_i386 -T linker.ld -nostdlib

# libgcc supplies the 64-bit math helpers (__udivdi3, __umoddi3, ...)
# that gcc emits on 32-bit when we do 64-bit / or % operations.  We're
# `-nostdlib` so ld won't find it via the default search; resolve the
# absolute path through gcc and pass it explicitly to the link.
LIBGCC  := $(shell $(CC) -m32 -print-libgcc-file-name)

C_SRCS := \
    kernel/core/kernel.c \
    kernel/core/shell.c \
    kernel/core/printf.c \
    kernel/core/multiboot.c \
    kernel/core/console.c \
    kernel/core/module.c \
    kernel/core/driver.c \
    kernel/core/config.c \
    kernel/core/syscall.c \
    kernel/core/task.c \
    kernel/core/block.c \
    kernel/core/block_cache.c \
    kernel/core/lock.c \
    kernel/core/vc.c \
    kernel/drivers/serial/serial.c \
    kernel/drivers/terminal/fb_terminal.c \
    kernel/drivers/terminal/vga_terminal.c \
    kernel/drivers/keyboard/ps2_keyboard.c \
    kernel/drivers/timer/pit.c \
    kernel/drivers/null/null.c \
    kernel/drivers/block/virtio_blk.c \
    kernel/drivers/usb/xhci.c \
    kernel/drivers/usb/usb_hid.c \
    kernel/acpi/acpi.c \
    kernel/fs/vfs.c \
    kernel/fs/ramfs.c \
    kernel/fs/devfs.c \
    kernel/fs/procfs.c \
    kernel/fs/exfat.c \
    kernel/mem/pmm.c \
    kernel/mem/vmm.c \
    kernel/mem/kmalloc.c \
    kernel/hal/x86/io.c \
    kernel/hal/x86/gdt.c \
    kernel/hal/x86/idt.c \
    kernel/hal/x86/tss.c \
    kernel/hal/x86/pci.c

ASM_SRCS := \
    kernel/hal/x86/boot.s \
    kernel/hal/x86/isr_stubs.s \
    kernel/hal/x86/usermode.s \
    kernel/hal/x86/switch.s

OBJS := $(C_SRCS:.c=.o) $(ASM_SRCS:.s=.o)

BUILD_DIR  := build
KERNEL_BIN := $(BUILD_DIR)/kernel.bin
ISO_DIR    := $(BUILD_DIR)/iso
ISO        := $(BUILD_DIR)/d-os.iso

.PHONY: all kernel iso run clean

all: $(KERNEL_BIN)

kernel: $(KERNEL_BIN)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL_BIN): $(OBJS) linker.ld
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

iso: $(ISO)

$(ISO): $(KERNEL_BIN) boot/grub/grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	cp boot/grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR)

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO)

clean:
	rm -f $(OBJS)
	rm -rf $(BUILD_DIR)
