#define NPROC       64  // プロセスの最大数
#define NCPU        8   // CPUの最大数
#define NOFILE      16  // プロセスあたりのオープン可能ファイル数
#define NFILE       100 // システム全体のオープン可能ファイル数
#define NINODE      50  // アクティブなiノードの最大数
#define NDEV        10  // メジャーデバイス番号の最大値
#define ROOTDEV     1   // ルートファイルシステムのディスクのデバイス番号
#define MAXARG      32  // exec引数の最大数
#define MAXOPBLOCKS 10  // 1回のFS操作が書き込む最大ブロック数
#define LOGBLOCKS (MAXOPBLOCKS * 3) // ディスク上のログ内の最大データブロック数
#define NBUF      (MAXOPBLOCKS * 3) // ディスクブロックキャッシュのサイズ
#define FSSIZE    2000              // ブロック単位のファイルシステムサイズ
#define MAXPATH   128               // ファイルパス名の最大長
#define USERSTACK 1                 // ユーザスタックのページ数
