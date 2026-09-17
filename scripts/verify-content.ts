import { existsSync } from 'node:fs';
import { readFile, readdir } from 'node:fs/promises';
import path from 'node:path';
import { BOOK_CHAPTERS, BOOK_CONTENT_DIR, BOOK_CONVERSION_STATS_PATH, BOOK_FIGURE_DIR, BOOK_JA_CONTENT_DIR, LABS, LAB_JA_PROCESSED_DIR, LAB_PROCESSED_DIR, PROJECT_ROOT } from './lib/config.ts';
import type { BookConversionRecord, ProcessedLab } from './lib/types.ts';

const failures: string[] = [];

for (const lab of LABS) {
  const file = path.join(LAB_PROCESSED_DIR, `${lab.slug}.json`);
  if (!existsSync(file)) {
    failures.push(`Missing processed Lab: ${lab.slug}`);
    continue;
  }
  const data = JSON.parse(await readFile(file, 'utf8')) as ProcessedLab;
  for (const key of ['h1', 'h2', 'h3', 'paragraphs', 'codeBlocks', 'links', 'lists'] as const) {
    if (data.renderedStats[key] < data.sourceStats[key]) {
      failures.push(`${lab.slug}: ${key} decreased ${data.sourceStats[key]} -> ${data.renderedStats[key]}`);
    }
  }
  const jaFile = path.join(LAB_JA_PROCESSED_DIR, `${lab.slug}.json`);
  if (!existsSync(jaFile)) failures.push(`Missing Japanese processed Lab: ${lab.slug}`);
  else {
    const ja = JSON.parse(await readFile(jaFile, 'utf8')) as ProcessedLab;
    if (!/[\u3040-\u30ff\u4e00-\u9fff]/u.test(ja.html)) failures.push(`${lab.slug}: Japanese translation is missing.`);
    if (ja.renderedStats.paragraphs < data.renderedStats.paragraphs || ja.renderedStats.codeBlocks < data.renderedStats.codeBlocks) failures.push(`${lab.slug}: Japanese copy lost protected content.`);
  }
}

if (!existsSync(BOOK_CONTENT_DIR)) {
  failures.push('Missing generated Book content directory.');
} else {
  const markdownFiles = (await readdir(BOOK_CONTENT_DIR)).filter((file) => file.endsWith('.md'));
  if (markdownFiles.length !== BOOK_CHAPTERS.length) {
    failures.push(`Expected ${BOOK_CHAPTERS.length} Book Markdown files, found ${markdownFiles.length}.`);
  }
  for (const file of markdownFiles) {
    const markdown = await readFile(path.join(BOOK_CONTENT_DIR, file), 'utf8');
    if (!/^---[\s\S]+title:[\s\S]+chapter:[\s\S]+---/m.test(markdown)) failures.push(`${file}: frontmatter missing.`);
    if ((markdown.match(/^##\s/gm) ?? []).length === 0 && !file.startsWith('00-') && !file.startsWith('13-')) failures.push(`${file}: no h2 headings.`);
    if (/\[@[^\]]+\]/.test(markdown)) failures.push(`${file}: unresolved bibliography citation.`);
    for (const match of markdown.matchAll(/src="\/book\/(fig\/[^"#?]+)"/g)) {
      if (!existsSync(path.join(PROJECT_ROOT, 'public/book', match[1]))) failures.push(`${file}: missing figure ${match[1]}.`);
    }
    for (const match of markdown.matchAll(/\]\((\.?\.?\/[^)#]+\.md)(?:#[^)]+)?\)/g)) {
      const target = path.resolve(BOOK_CONTENT_DIR, match[1]);
      if (!existsSync(target)) failures.push(`${file}: broken Markdown link ${match[1]}.`);
    }
  }
}

if (!existsSync(BOOK_JA_CONTENT_DIR)) failures.push('Missing Japanese Book content directory.');
else {
  const japaneseFiles = (await readdir(BOOK_JA_CONTENT_DIR)).filter((file) => file.endsWith('.md'));
  if (japaneseFiles.length !== BOOK_CHAPTERS.length) failures.push(`Expected ${BOOK_CHAPTERS.length} Japanese Book Markdown files, found ${japaneseFiles.length}.`);
  for (const file of japaneseFiles) {
    const markdown = await readFile(path.join(BOOK_JA_CONTENT_DIR, file), 'utf8');
    if (!/[\u3040-\u30ff\u4e00-\u9fff]/u.test(markdown)) failures.push(`${file}: Japanese translation is missing.`);
    if (/\[@[^\]]+\]/.test(markdown)) failures.push(`${file}: unresolved bibliography citation in Japanese copy.`);
    for (const match of markdown.matchAll(/src="\/book\/(fig\/[^"#?]+)"/g)) {
      if (!existsSync(path.join(PROJECT_ROOT, 'public/book', match[1]))) failures.push(`${file}: missing figure ${match[1]} in Japanese copy.`);
    }
  }
}

if (!existsSync(BOOK_FIGURE_DIR)) failures.push('Missing public Book figure directory.');

if (!existsSync(BOOK_CONVERSION_STATS_PATH)) {
  failures.push('Missing Book conversion statistics.');
} else {
  const stats = JSON.parse(await readFile(BOOK_CONVERSION_STATS_PATH, 'utf8')) as { chapters: BookConversionRecord[] };
  if (stats.chapters.length !== BOOK_CHAPTERS.length) failures.push('Book conversion statistics do not cover all chapters.');
  for (const chapter of stats.chapters) {
    for (const key of ['headings', 'codeBlocks', 'figures', 'tables', 'footnotes'] as const) {
      if (chapter.output[key] < chapter.source[key]) failures.push(`${chapter.file}: ${key} decreased ${chapter.source[key]} -> ${chapter.output[key]}.`);
    }
  }
}

if (failures.length > 0) {
  console.error(failures.map((failure) => `- ${failure}`).join('\n'));
  process.exitCode = 1;
} else {
  console.log('content: loss guards and local links passed');
}
