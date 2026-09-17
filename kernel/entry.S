        # qemu -kernelはカーネルを0x80000000に読み込み、
        # 各hart（つまりCPU）をそこへジャンプさせる。
        # kernel.ldは次のコードを
        # 0x80000000に配置する。
.section .text
.global _entry
_entry:
        # C用のスタックを設定する。
        # stack0はstart.cで宣言され、
        # CPUごとに4096バイトのスタックを持つ。
        # sp = stack0 + ((hartid + 1) * 4096)
        la sp, stack0
        li a0, 1024*4
        csrr a1, mhartid
        addi a1, a1, 1
        mul a0, a0, a1
        add sp, sp, a0
        # start.cのstart()へジャンプする
        call start
spin:
        j spin
