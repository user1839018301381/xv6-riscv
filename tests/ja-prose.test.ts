import assert from 'node:assert/strict';
import test from 'node:test';
import { polishJapaneseMarkdown, polishJapaneseProse } from '../scripts/lib/ja-prose.ts';

test('removes machine-translated spacing without touching information', () => {
  assert.equal(
    polishJapaneseProse('オペレーティング システムはページ テーブルを使います。 次に、処理を続けます。'),
    'オペレーティングシステムはページテーブルを使います。次に、処理を続けます。'
  );
});

test('normalizes recurring textbook terminology', () => {
  assert.equal(
    polishJapaneseProse('Xv6 の i ノードはバッファーに記録することができます。'),
    'xv6 の inode はバッファに記録できます。'
  );
});

test('splits only long prose paragraphs', () => {
  const sentence = 'カーネルはプロセスを管理し、各プロセスに独立したアドレス空間を割り当てます。';
  const prose = sentence.repeat(8);
  const code = `\`\`\`c\n${sentence.repeat(8)}\n\`\`\``;
  const result = polishJapaneseMarkdown(`${prose}\n\n${code}\n`);
  const [polishedProse, polishedCode] = result.split(`\n\n\`\`\`c`);

  assert.match(polishedProse, /\n\n/u);
  assert.equal(`\`\`\`c${polishedCode}`, `${code}\n`);
});
