// ユーザプロセス、カーネルスタック、ページテーブルページ、
// パイプバッファ用の物理メモリアロケータ。
// 4096バイト単位のページ全体を割り当てる。

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *start_address, void *end_address);

extern char end[]; // カーネル直後の最初のアドレス。
                   // kernel.ldで定義される。

struct run {
    struct run *next;
};

struct {
    struct spinlock lock;
    struct run *free_list;
} page_allocator;

void kinit()
{
    initlock(&page_allocator.lock, "page_allocator");
    freerange(end, (void *)PHYSTOP);
}

void freerange(void *start_address, void *end_address)
{
    char *page;
    page = (char *)PGROUNDUP((uint64)start_address);
    for (; page + PGSIZE <= (char *)end_address; page += PGSIZE)
        kfree(page);
}

// page_addressが指す物理メモリのページを解放する。
// 通常はkalloc()の呼び出しで返されたページである。
// （アロケータ初期化時は例外。上のkinitを参照。）
void kfree(void *page_address)
{
    struct run *free_page;

    if (((uint64)page_address % PGSIZE) != 0 ||
        (char *)page_address < end || (uint64)page_address >= PHYSTOP)
        panic("kfree");

    // ダングリング参照を検出できるようゴミで埋める。
    memset(page_address, 1, PGSIZE);

    free_page = (struct run *)page_address;

    acquire(&page_allocator.lock);
    free_page->next = page_allocator.free_list;
    page_allocator.free_list = free_page;
    release(&page_allocator.lock);
}

// 4096バイトの物理メモリページを1つ割り当てる。
// カーネルが使えるポインタを返す。
// メモリを割り当てられなければ0を返す。
void *kalloc(void)
{
    struct run *page;

    acquire(&page_allocator.lock);
    page = page_allocator.free_list;
    if (page)
        page_allocator.free_list = page->next;
    release(&page_allocator.lock);

    if (page)
        memset((char *)page, 5, PGSIZE); // ゴミで埋める
    return (void *)page;
}
