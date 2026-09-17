// 物理メモリ配置

// qemuの-machine virtは次のように設定されている。
// qemuのhw/riscv/virt.cに基づく:
//
//
// 00001000 -- ブートROM、qemuが提供
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtioディスク
// 80000000 -- qemuのブートROMはカーネルをここに読み込み、
//             ここへジャンプする。
// 80000000以降は未使用RAM。

// カーネルは物理メモリを次のように使う:
// 80000000 -- entry.S、続いてカーネルのテキストとデータ
// end -- カーネル用ページ割当て領域の開始
// PHYSTOP -- カーネルが使うRAMの終端

// qemuはUARTレジスタを物理メモリのここに配置する。
#define UART0     0x10000000L
#define UART0_IRQ 10

// virtio MMIOインタフェース
#define VIRTIO0     0x10001000
#define VIRTIO0_IRQ 1

// コアローカル割込みコントローラ（CLINT）
#define CLINT_BASE  0x02000000L
#define CLINT(hart) (CLINT_BASE + (hart) * 4)

// qemuはプラットフォームレベル割り込みコントローラ(PLIC)をここに配置する。
#define PLIC                 0x0c000000L
#define PLIC_PRIORITY        (PLIC + 0x0)
#define PLIC_PENDING         (PLIC + 0x1000)
#define PLIC_SENABLE(hart)   (PLIC + 0x2080 + (hart) * 0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart) * 0x2000)
#define PLIC_SCLAIM(hart)    (PLIC + 0x201004 + (hart) * 0x2000)

// カーネルは物理アドレス0x80000000からPHYSTOPまでに、
// カーネルページとユーザページ用のRAMがあることを想定する。
#define KERNBASE 0x80000000L
#define PHYSTOP  (KERNBASE + 128 * 1024 * 1024)

// ユーザ空間とカーネル空間の両方で、トランポリンページを最高アドレスにマップする。
#define TRAMPOLINE (MAXVA - PGSIZE)

// トランポリンの下にカーネルスタックをマップし、
// それぞれをアクセス不能なガードページで囲む。
#define KSTACK(p) (TRAMPOLINE - ((p) + 1) * 2 * PGSIZE)

// ユーザメモリの配置。
// アドレス0から順に:
//   テキスト
//   初期データとbss
//   固定サイズのスタック
//   拡張可能なヒープ
//   ...
//   TRAPFRAME (p->trapframe、トランポリンが使用)
//   TRAMPOLINE (カーネルと同じページ)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
