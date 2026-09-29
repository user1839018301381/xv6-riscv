---
title: "序文と謝辞"
chapter: 0
source: "xv6-riscv-book"
sourceFile: "acks.tex"
commit: "2d689eee47087bea267914123b8b1172583cbb0e"
---
これは、オペレーティングシステムに関するクラス向けのテキストの草案です。xv6 という名前のカーネル例を研究することで、オペレーティングシステムの主な概念を説明します。

xv6 は、Dennis Ritchie と Ken Thompson の Unix バージョン 6 (v6) (Ritchie and Thompson 1974) をモデルにしています。xv6 は v6 の構造とスタイルに大まかに準拠していますが、マルチコア RISC-V (Patterson and Waterman 2017) 用に ANSI C (Kernighan 1988) で実装されています。

このテキストは、John Lions の Commentary on UNIX 6th Edition (Lions 2000) に触発されたアプローチである xv6 のソースコードと一緒に読む必要があります。テキストには <https://github.com/mit-pdos/xv6-riscv> のソースコードへのハイパーリンクがあります。xv6 を使用したいくつかのラボ課題など、v6 および xv6 のオンラインリソースへの追加のポインタについては、<https://pdos.csail.mit.edu/6.1810> を参照してください。

このテキストは、MIT のオペレーティングシステムクラスである 6.828 および 6.1810 で使用されています。xv6 に直接的または間接的に貢献した教員、ティーチングアシスタント、およびクラスの学生に感謝します。特に、Adam Belay、Austin Clements、Niccolai Zeldovich に感謝します。最後に、本文のバグや改善の提案をメールで送ってくださった方々に感謝いたします: Abutalib Aghayev、Sebastian Boehm、brandb97、Anton Burtsev、Raphael Carvalho、Tej Chajed、Brendan Davidson、Rasit Eskicioglu、Color Fuzzy、Wojciech Gac、Giuseppe、Tao Guo、Haibo Hao、Naoki Hayama、Chrisヘンダーソン、ロバート・ヒルダーマン、エデン・ホッホバウム、ヴォルフガング・ケラー、パヴェウ・クラシェフスキ、ヘンリー・ライ、ジン・リー、オースティン・リュー、lyazj@github.com、パヴァン・マダムセッティ、ヤチェク・マシウラニエツ、マイケル・マッコンヴィル、m3hm00d、Mes0903、ミゲルグヴィエイラ、マーク・モリッシー、ムハメド・ムラド、ハリーパン、ハリー・ポーター、pr3pony、Siyuan Qian、Zhefeng Qiao、Askar Safin、Salman Shah、Huang Sha、Vikram Shenoy、Adeodato Simó、Ruslan Savchenko、Pawel Szczurko、Warren Toomey、tyfkda、tzerbib、unicornx、Vanush Vaswani、Chen Wang、Xi Wang、Zou Chang Wei、Sam Whitlock、Qiongsi Wu、LucyShawYang、ykf1114@gmail.com、Meng Zhou

間違いを見つけた場合、または改善のための提案がある場合は、Frans Kaashoek と Robert Morris (kaashoek,rtm@csail.mit.edu) に電子メールを送信してください。

## 参考文献
<span id="ref-kernighan"></span>
Kernighan、Brian W. 1988. *C プログラミング言語*。第2版編集はデニス・M・リッチー。プレンティスホールプロフェッショナルテクニカルリファレンス。

<span id="ref-lions"></span>
ライオンズ、ジョン。2000. *UNIX 第 6 版の解説*。ピアツーピア通信。

<span id="ref-riscv"></span>
パターソン、デイビッド、アンドリュー・ウォーターマン。2017. *RISC-V リーダー: オープンアーキテクチャアトラス*。ストロベリーキャニオン。

<span id="ref-unix"></span>
リッチー、デニス・M、ケン・トンプソン。1974.「UNIX タイムシェアリングシステム」。*共通。ACM* 17 (7): 365--75。<https://doi.org/10.1145/361011.361061>。