import { readFile, readdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { PROJECT_ROOT } from './lib/config.ts';
import { polishJapaneseMarkdown } from './lib/ja-prose.ts';

const bookDirectory = path.join(PROJECT_ROOT, 'content/book-ja');
const files = (await readdir(bookDirectory)).filter((name) => name.endsWith('.md')).sort();

for (const file of files) {
  const filePath = path.join(bookDirectory, file);
  const source = await readFile(filePath, 'utf8');
  const polished = polishJapaneseMarkdown(source);
  if (polished === source) continue;
  await writeFile(filePath, polished);
  console.log(`book-ja/${file}: polished`);
}
