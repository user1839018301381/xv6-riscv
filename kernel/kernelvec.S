        #
        # スーパーバイザモード中の割り込みと例外はここへ来る。
        #
        # 現在のスタックはカーネルスタックである。
        # レジスタを積んでkerneltrap()を呼ぶ。
        # kerneltrap()から戻ったらレジスタを復元して戻る。
        #
.globl kerneltrap
.globl kernelvec
.align 4
kernelvec:
        # レジスタ保存用の空間を作る。
        addi sp, sp, -256

        # 呼び出し元保存レジスタを保存する。
        sd ra, 0(sp)
        # sd sp, 8(sp)
        sd gp, 16(sp)
        # sd tp, 24(sp)
        sd t0, 32(sp)
        sd t1, 40(sp)
        sd t2, 48(sp)
        sd a0, 72(sp)
        sd a1, 80(sp)
        sd a2, 88(sp)
        sd a3, 96(sp)
        sd a4, 104(sp)
        sd a5, 112(sp)
        sd a6, 120(sp)
        sd a7, 128(sp)
        sd t3, 216(sp)
        sd t4, 224(sp)
        sd t5, 232(sp)
        sd t6, 240(sp)

        # trap.cのCトラップハンドラを呼ぶ
        call kerneltrap

        # レジスタを復元する。
        ld ra, 0(sp)
        # ld sp, 8(sp)
        ld gp, 16(sp)
        # CPUを移動した場合に備え、hartidを含むtpは復元しない
        ld t0, 32(sp)
        ld t1, 40(sp)
        ld t2, 48(sp)
        ld a0, 72(sp)
        ld a1, 80(sp)
        ld a2, 88(sp)
        ld a3, 96(sp)
        ld a4, 104(sp)
        ld a5, 112(sp)
        ld a6, 120(sp)
        ld a7, 128(sp)
        ld t3, 216(sp)
        ld t4, 224(sp)
        ld t5, 232(sp)
        ld t6, 240(sp)

        addi sp, sp, 256

        # カーネルで行っていた処理へ戻る。
        sret
