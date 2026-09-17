        #
        # ユーザ空間からカーネルへのトラップを処理し、
        # カーネルからユーザへ戻る低レベルコード。
        #
        # カーネルはこのコードを含むページをユーザ空間とカーネル空間で
        # 同じ仮想アドレス（TRAMPOLINE）にマップするため、
        # ページテーブルを切り替えても動作し続ける。
        # kernel.ldはこのコードがページ境界から始まるようにする。
        #

#include "riscv.h"
#include "memlayout.h"

.section trampsec
.globl trampoline
.globl usertrap
trampoline:
.align 4
.globl uservec
uservec:    
	#
        # trap.cはstvecがここを指すよう設定する。
        # そのためユーザ空間からのトラップは、
        # スーパーバイザモードで、ユーザページテーブルを使って
        # ここから始まる。
        #

        # ユーザのa0をsscratchに保存し、
        # a0をTRAPFRAME取得用に使えるようにする。
        csrw sscratch, a0

        # 各プロセスは別々のp->trapframeメモリ領域を持つが、
        # 各プロセスのユーザページテーブルでは同じ仮想アドレス
        # （TRAPFRAME）にマップされる。
        li a0, TRAPFRAME
        
        # ユーザレジスタをTRAPFRAMEに保存する
        sd ra, 40(a0)
        sd sp, 48(a0)
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)

	# ユーザのa0をp->trapframe->a0に保存する
        csrr t0, sscratch
        sd t0, 112(a0)

        # p->trapframe->kernel_spからカーネルスタックポインタを初期化する
        ld sp, 8(a0)

        # p->trapframe->kernel_hartidから現在のhartidをtpに設定する
        ld tp, 32(a0)

        # p->trapframe->kernel_trapからusertrap()のアドレスを読み込む
        ld t0, 16(a0)

        # p->trapframe->kernel_satpからカーネルページテーブルのアドレスを取得する。
        ld t1, 0(a0)

        # 先行するメモリ操作がユーザページテーブルを使うよう、完了を待つ。
        sfence.vma zero, zero

        # カーネルページテーブルを設定する。
        csrw satp, t1

        # 古くなったユーザエントリをTLBからフラッシュする。
        sfence.vma zero, zero

        # usertrap()を呼ぶ
        jalr t0

.globl userret
userret:
        # usertrap()はユーザsatpをa0に入れてここへ戻る。
        # カーネルからユーザへ戻る。

        # このhartでこのプロセスを実行するのが初めての場合に備え、
        # 命令キャッシュをフラッシュする。
        fence.i

        # ユーザページテーブルへ切り替える。
        sfence.vma zero, zero
        csrw satp, a0
        sfence.vma zero, zero

        li a0, TRAPFRAME

        # a0以外をTRAPFRAMEから復元する
        ld ra, 40(a0)
        ld sp, 48(a0)
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

	# ユーザのa0を復元する
        ld a0, 112(a0)
        
        # ユーザモードとユーザpcへ戻る。
        # prepare_return()がsstatusとsepcを設定している。
        sret
