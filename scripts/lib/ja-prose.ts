/**
 * Remove recurring machine-translation artifacts without changing the facts,
 * symbols, links, or code embedded in the source material.
 */
export function polishJapaneseProse(value: string): string {
  let result = value
    .replace(/Chapter(?:Â|\u00a0)?\s*(?=\[)/g, '第')
    .replace(/(?<=[ァ-ヶー]) (?=[ァ-ヶー])/gu, '')
    .replace(/[iI] ノード/g, 'inode')
    .replace(/\binode(?=[ぁ-んァ-ヶ一-龠])/gu, 'inode ')
    .replace(/インターフェイス/g, 'インターフェース')
    .replace(/メモリー/g, 'メモリ')
    .replace(/バッファー/g, 'バッファ')
    .replace(/Xv6/g, 'xv6')
    .replace(/。 +(?=\S)/gu, '。')
    .replace(/、 +(?=\S)/gu, '、')
    .replace(/することができます/g, 'できます')
    .replace(/することができる/g, 'できる')
    .replace(/することができません/g, 'できません')
    .replace(/未発見の種族/g, '未発見の競合')
    .replace(/発見されていない種族/g, '未発見の競合')
    .replace(/`wakeup` に 1 回電話をかける/g, '`wakeup` を1回呼び出す')
    .replace(/子供の記憶/g, '子プロセスのメモリ')
    .replace(/子供が出るのを待ちます。\\\*status の終了ステータス;子 PID を返します。/g, '子プロセスが終了するまで待ちます。終了ステータスを \\*status に格納し、子プロセスの PID を返します。')
    .replace(/現在のプロセスを終了します。ステータスは wait\(\) に報告されます。返品はできません。/g, '現在のプロセスを終了します。ステータスは `wait()` に報告されます。この関数は戻りません。')
    .replace(/ファイル終了信号/g, 'EOF（ファイル終端）')
    .replace(/人種を識別するツール/g, '競合を検出するツール')
    .replace(/記憶バリア/g, 'メモリバリア')
    .replace(/^記憶$/gmu, 'メモリ')
    .replace(/ロックプラン/g, 'ロック方式')
    .replace(/機密性の高い更新シーケンス/g, '整合性が重要な更新処理')
    .replace(/強制的に強制終了/g, '強制終了')
    .replace(/新しいファイルの作成ができることのほんの一部にすぎないため/g, '新規ファイルの作成は `sys_open` の機能の一部にすぎないため')
    .replace(/リンクが解除される \(削除される\) ファイルの賢明な処理/g, 'リンクが解除された（削除された）ファイルを適切に処理')
    .replace(/正しいことを自分自身に納得させる必要があります/g, '並行実行しても正しいことを確認しなければなりません')
    .replace(/今になって考えると、この計画は明白に思えますが/g, '現在では当然に見える設計ですが');

  result = result.replace(/\[(\d+)\] \((\/book\/[^)]+)\)/gu, '[$1]($2)');

  return result;
}

function splitLongParagraph(value: string): string {
  if (value.length < 260 || (value.match(/。/gu)?.length ?? 0) < 4) return value;
  if (/<\/?[A-Za-z][^>]*>/u.test(value)) return value;
  if (/^(?:\s*#{1,6}\s|\s*[-*+]\s|\s*\d+\.\s|\s*>| {4}|\t)/mu.test(value)) return value;

  const sentences = value.match(/[^。]*。|[^。]+$/gu) ?? [value];
  const paragraphs: string[] = [];
  let paragraph = '';

  for (const sentence of sentences) {
    paragraph += sentence;
    if (paragraph.length >= 180) {
      paragraphs.push(paragraph.trim());
      paragraph = '';
    }
  }
  if (paragraph.trim()) paragraphs.push(paragraph.trim());

  return paragraphs.length > 1 ? paragraphs.join('\n\n') : value;
}

/** Apply prose cleanup to Markdown while leaving fenced code unchanged. */
export function polishJapaneseMarkdown(value: string): string {
  const fencedBlocks: string[] = [];
  const protectedValue = value.replace(/```[\s\S]*?```/gu, (block) => {
    const token = `XV6_FENCED_BLOCK_${fencedBlocks.length}`;
    fencedBlocks.push(block);
    return token;
  });

  let result = protectedValue
    .split(/\n{2,}/u)
    .map((block) => splitLongParagraph(polishJapaneseProse(block)))
    .join('\n\n');

  result = result.replace(/XV6_FENCED_BLOCK_(\d+)/gu, (_match, index: string) => fencedBlocks[Number(index)] ?? _match);
  return value.endsWith('\n') && !result.endsWith('\n') ? `${result}\n` : result;
}
