import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { readFile, readdir } from 'node:fs/promises';
import path from 'node:path';
import test from 'node:test';
import {
  BOOK_CHAPTERS,
  BOOK_CONTENT_DIR,
  BOOK_JA_CONTENT_DIR,
  BOOK_CONVERSION_STATS_PATH,
  LABS,
  LAB_JA_PROCESSED_DIR,
  LAB_METADATA_PATH,
  LAB_PROCESSED_DIR,
  LAB_RAW_DIR,
  PROJECT_ROOT
} from '../scripts/lib/config.ts';
import { restoreLabCodeHeadings, restoreLabTitle } from '../scripts/lib/ja-code.ts';
import type { BookConversionRecord, LabMetadataFile, ProcessedLab } from '../scripts/lib/types.ts';

test('the cached Lab set is exactly the Fall 2026 allowlist', async () => {
  const expected = LABS.map((lab) => lab.slug);
  const metadata = JSON.parse(await readFile(LAB_METADATA_PATH, 'utf8')) as LabMetadataFile;
  assert.deepEqual(metadata.allowlist, expected);
  assert.deepEqual(
    (await readdir(LAB_RAW_DIR)).filter((name) => name.endsWith('.html')).map((name) => name.replace(/\.html$/, '')).sort(),
    [...expected].sort()
  );

  for (const slug of expected) {
    const processed = JSON.parse(await readFile(path.join(LAB_PROCESSED_DIR, `${slug}.json`), 'utf8')) as ProcessedLab;
    assert.equal(processed.sourceStats.h1, processed.renderedStats.h1);
    assert.equal(processed.sourceStats.paragraphs, processed.renderedStats.paragraphs);
    assert.equal(processed.sourceStats.codeBlocks, processed.renderedStats.codeBlocks);
    assert.equal(processed.sourceStats.links, processed.renderedStats.links);
  }
});

test('all official Book chapters and referenced figures are present', async () => {
  const files = (await readdir(BOOK_CONTENT_DIR)).filter((name) => name.endsWith('.md')).sort();
  assert.equal(files.length, BOOK_CHAPTERS.length);

  for (const file of files) {
    const markdown = await readFile(path.join(BOOK_CONTENT_DIR, file), 'utf8');
    assert.match(markdown, /^---[\s\S]+commit: "[0-9a-f]{40}"[\s\S]+---/);
    assert.doesNotMatch(markdown, /\[@[^\]]+\]/, `${file} has an unresolved citation`);
    for (const match of markdown.matchAll(/src="\/book\/(fig\/[^"?#]+)"/g)) {
      assert.ok(existsSync(path.join(PROJECT_ROOT, 'public/book', match[1])), `${file} references missing ${match[1]}`);
    }
  }
});

test('Book heading and code-block counts do not decrease during conversion', async () => {
  const stats = JSON.parse(await readFile(BOOK_CONVERSION_STATS_PATH, 'utf8')) as { chapters: BookConversionRecord[] };
  assert.equal(stats.chapters.length, BOOK_CHAPTERS.length);
  for (const chapter of stats.chapters) {
    assert.ok(chapter.output.headings >= chapter.source.headings, `${chapter.file} lost headings`);
    assert.ok(chapter.output.codeBlocks >= chapter.source.codeBlocks, `${chapter.file} lost code blocks`);
  }
});

test('Japanese copies cover every Lab and Book page', async () => {
  for (const lab of LABS) {
    const translated = JSON.parse(await readFile(path.join(LAB_JA_PROCESSED_DIR, `${lab.slug}.json`), 'utf8')) as ProcessedLab;
    assert.match(translated.html, /[\u3040-\u30ff\u4e00-\u9fff]/u, `${lab.slug} is not translated`);
    assert.equal(translated.outline.length > 0, true, `${lab.slug} lost its outline`);
    assert.doesNotMatch(translated.html, /ZQX9|XV6PROTECT|XV6COMMENT/u, `${lab.slug} contains a translation placeholder`);
  }
  const translatedBooks = (await readdir(BOOK_JA_CONTENT_DIR)).filter((file) => file.endsWith('.md'));
  assert.equal(translatedBooks.length, BOOK_CHAPTERS.length);
  for (const file of translatedBooks) {
    const markdown = await readFile(path.join(BOOK_JA_CONTENT_DIR, file), 'utf8');
    assert.match(markdown, /[\u3040-\u30ff\u4e00-\u9fff]/u, `${file} is not translated`);
    assert.doesNotMatch(markdown, /ZQX9|XV6PROTECT|XV6COMMENT/u, `${file} contains a translation placeholder`);
  }
});

test('Japanese Book keeps API and source identifiers in English', async () => {
  const interfaces = await readFile(path.join(BOOK_JA_CONTENT_DIR, '01-operating-system-interfaces.md'), 'utf8');
  assert.match(interfaces, /int fork\(\)/u);
  assert.match(interfaces, /int exit\(int status\)/u);
  assert.match(interfaces, /int pause\(int n\)/u);
  assert.match(interfaces, /int pipe\(int p\\\[\\\]\)/u);
  assert.doesNotMatch(interfaces, /int (?:フォーク|一時停止|パイプ)\(/u);

  const organization = await readFile(path.join(BOOK_JA_CONTENT_DIR, '02-operating-system-organization.md'), 'utf8');
  for (const file of ['entry.S', 'exec.c', 'swtch.S', 'trampoline.S', 'syscall.c', 'console.c', 'printk.c', 'bio.c', 'sleeplock.c']) {
    assert.match(organization, new RegExp(`\\b${file.replace('.', '\\.') }\\b`));
  }

  const locking = await readFile(path.join(BOOK_JA_CONTENT_DIR, '07-locking.md'), 'utf8');
  for (const lock of ['bcache.lock', 'cons.lock', 'log.lock', 'pipe\'s pi-\\>lock', 'tickslock', 'buf\'s b-\\>lock']) {
    assert.match(locking, new RegExp(lock.replace(/[\\^$.*+?()[\]{}|]/g, '\\$&')));
  }
});

test('Japanese Lab keeps executable names in headings and titles', async () => {
  const cow = JSON.parse(await readFile(path.join(LAB_JA_PROCESSED_DIR, 'cow.json'), 'utf8')) as ProcessedLab;
  assert.match(restoreLabTitle(cow.title), /コピーオンライト fork/u);
  assert.match(restoreLabCodeHeadings(cow.html), /id="implement-copy-on-write-fork"[^>]*>コピーオンライト fork の実装<\/h2>/u);

  const util = JSON.parse(await readFile(path.join(LAB_JA_PROCESSED_DIR, 'util.json'), 'utf8')) as ProcessedLab;
  const normalized = restoreLabCodeHeadings(util.html);
  for (const name of ['sleep', 'sixfive', 'memdump', 'find', 'exec']) {
    assert.match(normalized, new RegExp(`id="${name}"[^>]*>${name}<\\/h2>`));
  }
});

test('the generated search index covers Labs and Book sections', async () => {
  const index = JSON.parse(await readFile(path.join(PROJECT_ROOT, 'public/search-index.json'), 'utf8')) as {
    records: Array<{ kind: string; url: string }>;
  };
  assert.ok(index.records.length >= 150);
  assert.ok(index.records.some((record) => record.kind === 'Lab' && record.url.startsWith('/labs/')));
  assert.ok(index.records.some((record) => record.kind === 'Book' && record.url.startsWith('/book/chapter-')));
  assert.ok(index.records.some((record) => record.kind === 'Book' && record.url === '/book/chapter-2/section-1/'));
});
