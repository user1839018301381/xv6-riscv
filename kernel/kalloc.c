// ユーザプロセス、カーネルスタック、ページテーブルページ、
// パイプバッファ用の物理メモリアロケータ。
// 4096バイト単位のページ全体を割り当てる。

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // カーネル直後の最初のアドレス。
                   // kernel.ldで定義される。

struct run {
    struct run *next;
};

struct {
    struct spinlock lock;
    struct run *freelist;
} kmem;

void kinit()
{
    initlock(&kmem.lock, "kmem");
    freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
    char *p;
    p = (char *)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
        kfree(p);
}

// paが指す物理メモリのページを解放する。
// 通常はkalloc()の呼び出しで返されたページである。
// （アロケータ初期化時は例外。上のkinitを参照。）
void kfree(void *pa)
{
    struct run *r;

    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

    // ダングリング参照を検出できるようゴミで埋める。
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
}

// 4096バイトの物理メモリページを1つ割り当てる。
// カーネルが使えるポインタを返す。
// メモリを割り当てられなければ0を返す。
void *kalloc(void)
{
    struct run *r;

    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r)
        kmem.freelist = r->next;
    release(&kmem.lock);

    if (r)
        memset((char *)r, 5, PGSIZE); // ゴミで埋める
    return (void *)r;
}
