import { existsSync } from 'node:fs';
import { mkdir, readFile, readdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { load } from 'cheerio';
import { BOOK_CONTENT_DIR, BOOK_JA_CONTENT_DIR, LAB_JA_PROCESSED_DIR, LAB_PROCESSED_DIR, PROJECT_ROOT, chapterPath, sectionPath } from './lib/config.ts';
import { restoreLabCodeHeadings, restoreLabTitle } from './lib/ja-code.ts';
import type { ProcessedLab, SearchRecord } from './lib/types.ts';

function compactText(value: string): string {
  return value.replace(/\s+/g, ' ').trim();
}

function tokens(value: string): string {
  return compactText(value).toLocaleLowerCase('en-US');
}

function labRecords(lab: ProcessedLab): SearchRecord[] {
  const normalizedTitle = restoreLabTitle(lab.title);
  const $ = load(`<article>${restoreLabCodeHeadings(lab.html)}</article>`, null, false);
  const records: SearchRecord[] = [];
  const headings = $('h1, h2, h3').toArray();
  headings.forEach((heading, index) => {
    const current = $(heading);
    const parts: string[] = [];
    let node = current.next();
    while (node.length && !/^h[1-3]$/i.test(node.get(0)?.tagName ?? '')) {
      parts.push(node.text());
      node = node.next();
    }
    const section = compactText(current.text());
    const text = compactText(parts.join(' '));
    const id = current.attr('id');
    records.push({
      id: `lab-${lab.slug}-${index}`,
      kind: 'Lab',
      collection: lab.slug,
      title: normalizedTitle,
      section,
      url: `/labs/${lab.slug}/${id ? `#${id}` : ''}`,
      text,
      tokens: tokens(`${lab.slug} ${normalizedTitle} ${section} ${text}`)
    });
  });
  return records;
}

function parseFrontmatter(markdown: string): { data: Record<string, string>; body: string } {
  const match = markdown.match(/^---\n([\s\S]*?)\n---\n([\s\S]*)$/);
  if (!match) return { data: {}, body: markdown };
  const data: Record<string, string> = {};
  for (const line of match[1].split('\n')) {
    const separator = line.indexOf(':');
    if (separator < 0) continue;
    const key = line.slice(0, separator).trim();
    const value = line.slice(separator + 1).trim().replace(/^"|"$/g, '');
    data[key] = value;
  }
  return { data, body: match[2] };
}

function bookRecords(markdown: string, fileName: string): SearchRecord[] {
  const { data, body } = parseFrontmatter(markdown);
  const chapter = Number(data.chapter);
  const title = data.title || fileName;
  const lines = body.split('\n');
  const records: SearchRecord[] = [];
  let section = title;
  let anchor = '';
  let buffer: string[] = [];
  let index = 0;
  let sectionNumber = 0;
  let sectionUrl = chapterPath(chapter);

  const flush = () => {
    const text = compactText(buffer.join(' ').replace(/<[^>]+>/g, ' ').replace(/[`*_>#|]/g, ' '));
    if (!text && records.length > 0) return;
    records.push({
      id: `book-${chapter}-${index++}`,
      kind: 'Book',
      collection: `Chapter ${chapter}`,
      title,
      section,
      url: `${sectionUrl}${anchor ? `#${anchor}` : ''}`,
      text,
      tokens: tokens(`chapter ${chapter} ${title} ${section} ${text}`)
    });
    buffer = [];
  };

  for (const line of lines) {
    const anchorMatch = line.match(/^<a id="([^"]+)"><\/a>$/);
    if (anchorMatch) {
      anchor = anchorMatch[1];
      continue;
    }
    const headingMatch = line.match(/^#{2,3}\s+(.+)$/);
    if (headingMatch) {
      flush();
      if (line.startsWith('## ')) {
        sectionNumber += 1;
        sectionUrl = sectionPath(chapter, sectionNumber);
      }
      section = compactText(headingMatch[1]);
      anchor = '';
      continue;
    }
    buffer.push(line);
  }
  flush();
  return records;
}

const records: SearchRecord[] = [];
const labSearchDir = existsSync(LAB_JA_PROCESSED_DIR) ? LAB_JA_PROCESSED_DIR : LAB_PROCESSED_DIR;
if (existsSync(labSearchDir)) {
  for (const file of (await readdir(labSearchDir)).filter((name) => name.endsWith('.json')).sort()) {
    records.push(...labRecords(JSON.parse(await readFile(path.join(labSearchDir, file), 'utf8')) as ProcessedLab));
  }
}
const bookSearchDir = existsSync(BOOK_JA_CONTENT_DIR) ? BOOK_JA_CONTENT_DIR : BOOK_CONTENT_DIR;
if (existsSync(bookSearchDir)) {
  for (const file of (await readdir(bookSearchDir)).filter((name) => name.endsWith('.md')).sort()) {
    records.push(...bookRecords(await readFile(path.join(bookSearchDir, file), 'utf8'), file));
  }
}

const output = path.join(PROJECT_ROOT, 'public/search-index.json');
await mkdir(path.dirname(output), { recursive: true });
await writeFile(output, `${JSON.stringify({ version: 1, generatedAt: new Date().toISOString(), records })}\n`);
console.log(`search: ${records.length} section records`);
