K=kernel
U=user

OBJS = \
  $K/entry.o \
  $K/start.o \
  $K/console.o \
  $K/printk.o \
  $K/uart.o \
  $K/kalloc.o \
  $K/spinlock.o \
  $K/string.o \
  $K/main.o \
  $K/vm.o \
  $K/proc.o \
  $K/swtch.o \
  $K/trampoline.o \
  $K/trap.o \
  $K/syscall.o \
  $K/sysproc.o \
  $K/bio.o \
  $K/fs.o \
  $K/log.o \
  $K/sleeplock.o \
  $K/file.o \
  $K/pipe.o \
  $K/exec.o \
  $K/sysfile.o \
  $K/kernelvec.o \
  $K/plic.o \
  $K/virtio_disk.o

# riscv64-unknown-elf-またはriscv64-linux-gnu-。
# /opt/riscv/binにあるかもしれない。
#TOOLPREFIX = 

# 未設定なら正しいTOOLPREFIXを推測する
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-elf-'; \
	elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-elf-'; \
	elif riscv64-none-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-none-elf-'; \
	elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-linux-gnu-'; \
	elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-linux-gnu-'; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

QEMU = qemu-system-riscv64
MIN_QEMU_VERSION = 7.2

CC = $(TOOLPREFIX)gcc
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

# 再現可能なビルド。
DETFLAGS = -ffile-prefix-map=$(CURDIR)=.

CFLAGS = -Wall -Werror -Wno-unknown-attributes -O -fno-omit-frame-pointer -ggdb -gdwarf-2
CFLAGS += $(DETFLAGS)
CFLAGS += -march=rv64gc
CFLAGS += -std=gnu99
CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding
CFLAGS += -fno-common -nostdlib
CFLAGS += -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset
CFLAGS += -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero
CFLAGS += -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc
CFLAGS += -fno-builtin-free
CFLAGS += -fno-builtin-memcpy -Wno-main
CFLAGS += -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf
CFLAGS += -I.
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# 可能ならPIEを無効にする（Ubuntu 16.10のツールチェーン用）
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

LDFLAGS = -z max-page-size=4096

$K/kernel: $(OBJS) $K/kernel.ld
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o $K/kernel $(OBJS) 
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$K/%.o: $K/%.S
	$(CC) -march=rv64gc -g $(DETFLAGS) -c -o $@ $<

tags: $(OBJS)
	etags kernel/*.S kernel/*.c

ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o

_%: %.o $(ULIB) $U/user.ld
	$(LD) $(LDFLAGS) -T $U/user.ld -o $@ $< $(ULIB)
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S

$U/_forktest: $U/forktest.o $(ULIB)
	# forktestはリンクするライブラリコードが少ないため、
	# プロセス表を満杯にできるよう小さくする必要がある。
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $U/_forktest $U/forktest.o $U/ulib.o $U/usys.o
	$(OBJDUMP) -S $U/_forktest > $U/forktest.asm

mkfs/mkfs: mkfs/mkfs.c $K/fs.h $K/param.h
	gcc -Wno-unknown-attributes -I. -o mkfs/mkfs mkfs/mkfs.c

# 最初のビルド後にcat.oなどの中間ファイルが削除されるのを防ぎ、
# 最初のビルド後のディスクイメージ変更をcleanまで保持する。詳細:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o

UPROGS=\
	$U/_cat\
	$U/_echo\
	$U/_forktest\
	$U/_grep\
	$U/_init\
	$U/_kill\
	$U/_ln\
	$U/_ls\
	$U/_mkdir\
	$U/_rm\
	$U/_sh\
	$U/_stressfs\
	$U/_usertests\
	$U/_grind\
	$U/_wc\
	$U/_zombie\
	$U/_logstress\
	$U/_forphan\
	$U/_dorphan\
	$U/_sync\

fs.img: mkfs/mkfs README $(UPROGS)
	mkfs/mkfs fs.img README $(UPROGS)

-include kernel/*.d user/*.d

clean: 
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym \
	$K/kernel fs.img \
	mkfs/mkfs .gdbinit \
        $U/usys.S \
	$(UPROGS)

# 一意なGDBポートを生成しようとする
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMUのgdbスタブのコマンドラインは0.11で変更された
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 3
endif

QEMUOPTS = -machine virt -bios none -kernel $K/kernel -m 128M -smp $(CPUS) -nographic
QEMUOPTS += -global virtio-mmio.force-legacy=false
QEMUOPTS += -drive file=fs.img,if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

qemu: check-qemu-version $K/kernel fs.img
	$(QEMU) $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: $K/kernel .gdbinit fs.img
	@echo "*** Now run 'gdb' in another window." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

print-gdbport:
	@echo $(GDBPORT)

QEMU_VERSION := $(shell $(QEMU) --version | head -n 1 | sed -E 's/^QEMU emulator version ([0-9]+\.[0-9]+)\..*/\1/')
check-qemu-version:
	@if [ "$(shell echo "$(QEMU_VERSION) >= $(MIN_QEMU_VERSION)" | bc)" -eq 0 ]; then \
		echo "ERROR: Need qemu version >= $(MIN_QEMU_VERSION)"; \
		exit 1; \
	fi

# 便利エイリアス: make kernel / make mkfs
kernel: $K/kernel
mkfs: mkfs/mkfs

test: ## xv6 の全テストを実行する
	./test-xv6.py usertests
	./test-xv6.py crash

test-exec: ## exec のテストを実行する
	./test-xv6.py exectest
	./test-xv6.py bigargtest
	./test-xv6.py execout

# xv6 Reader (Astro 静的サイト)
DOCS_PORT ?= 4321
docs: ## Reader を起動してブラウザで開く (http://localhost:4321)
	@if [ ! -d node_modules ]; then echo "Installing dependencies..."; npm install; fi
	npm run dev -- --port $(DOCS_PORT) --open

.PHONY: help fmt docs kernel mkfs test test-exec tags clean qemu qemu-gdb print-gdbport check-qemu-version
help: ## 利用可能なターゲットと変数の一覧を表示する
	@echo "Usage: make [target] [VAR=value]"
	@echo ""
	@echo "Targets:"
	@echo "  qemu               QEMU で xv6 を起動する"
	@echo "  qemu-gdb           GDB 接続待ちで QEMU を起動する (別窓で gdb)"
	@echo "  kernel             カーネル ($K/kernel) をビルドする"
	@echo "  fs.img             ファイルシステムイメージをビルドする"
	@echo "  mkfs               ホスト用 mkfs/mkfs をビルドする"
	@echo "  test               xv6 の全テストを実行する"
	@echo "  test-exec          exec のテストを実行する"
	@echo "  tags               kernel の etags を生成する"
	@echo "  clean              ビルド成果物を削除する"
	@echo "  fmt                clang-format で整形する"
	@echo "  docs               xv6 Reader を起動してブラウザで開く"
	@echo "  print-gdbport      GDB 用ポート番号を表示する"
	@echo "  help               このヘルプを表示する"
	@echo ""
	@echo "Variables:"
	@echo "  TOOLPREFIX=...    RISC-V ツールチェイン接頭辞 (未設定なら自動検出)"
	@echo "  CPUS=N            QEMU の CPU 数 (既定: 3)"
	@echo "  QEMU=...          QEMU バイナリ (既定: qemu-system-riscv64)"
	@echo "  DOCS_PORT=N       Reader のポート (既定: 4321)"
fmt:
	clang-format -i $(wildcard kernel/*.[ch] user/*.[ch] mkfs/*.c)
