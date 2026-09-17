#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

// start()から、スーパーバイザモードで全CPUがここへジャンプする。
void main()
{
    if (cpuid() == 0) {
        consoleinit();
        printkinit();
        printk("\n");
        printk("xv6 kernel is booting\n");
        printk("\n");
        kinit();            // 物理ページアロケータ
        kvminit();          // カーネルページテーブルを作成する
        kvminithart();      // ページングを有効にする
        procinit();         // プロセステーブル
        trapinit();         // トラップベクタ
        trapinithart();     // カーネルトラップベクタを導入する
        plicinit();         // 割り込みコントローラを初期設定する
        plicinithart();     // デバイス割り込みをPLICに要求する
        binit();            // バッファキャッシュ
        iinit();            // inodeテーブル
        fileinit();         // ファイルテーブル
        virtio_disk_init(); // エミュレートされたハードディスク
        userinit();         // 最初のユーザプロセス

        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) == 0)
            ;

        printk("hart %d starting\n", cpuid());
        kvminithart();  // ページングを有効にする
        trapinithart(); // カーネルトラップベクタを導入する
        plicinithart(); // デバイス割り込みをPLICに要求する
    }

    scheduler();
}
