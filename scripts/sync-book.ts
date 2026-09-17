import { execFile, spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { cp, mkdir, readdir, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';
import {
  BOOK_CHAPTERS,
  BOOK_CONVERSION_STATS_PATH,
  BOOK_CONTENT_DIR,
  BOOK_FIGURE_DIR,
  BOOK_REPOSITORY,
  BOOK_SOURCE_DIR,
  BOOK_VERSION_PATH,
  chapterPath
} from './lib/config.ts';
import type { BookConversionRecord, BookMarkdownStats } from './lib/types.ts';

const execFileAsync = promisify(execFile);
const refresh = process.argv.slice(2).includes('--refresh');
const sourceParent = path.dirname(BOOK_SOURCE_DIR);
const temporarySource = `${BOOK_SOURCE_DIR}.tmp`;
const xv6Source = path.join(BOOK_SOURCE_DIR, 'xv6-riscv-src');
const latexOutput = path.join(BOOK_SOURCE_DIR, 'latex.out');
const quartoOutput = path.join(BOOK_SOURCE_DIR, 'quarto.out');
const publishedBookBase = 'https://mit-pdos.github.io/xv6-riscv-book/';
const generatedFigures = ['fslayer.svg', 'fslayout.svg', 'order.svg', 'switch.svg', 'trap.svg'] as const;

async function run(command: string, args: string[], options: { cwd?: string; env?: NodeJS.ProcessEnv; tolerateFailure?: boolean } = {}): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    const child = spawn(command, args, {
      cwd: options.cwd,
      env: options.env,
      stdio: 'inherit'
    });
    child.on('error', reject);
    child.on('exit', (code) => {
      if (code === 0 || options.tolerateFailure) resolve();
      else reject(new Error(`${command} exited with status ${code}`));
    });
  });
}

async function ensureBookRepository(): Promise<void> {
  await mkdir(sourceParent, { recursive: true });
  if (!existsSync(BOOK_SOURCE_DIR)) {
    await rm(temporarySource, { recursive: true, force: true });
    await run('git', ['clone', '--depth', '1', BOOK_REPOSITORY, temporarySource]);
    await run('mv', [temporarySource, BOOK_SOURCE_DIR]);
    return;
  }
  if (refresh) {
    await run('git', ['-C', BOOK_SOURCE_DIR, 'pull', '--ff-only']);
  }
}

async function sourceCommit(): Promise<string> {
  const { stdout } = await execFileAsync('git', ['-C', BOOK_SOURCE_DIR, 'rev-parse', 'HEAD']);
  return stdout.trim();
}

async function ensureXv6Source(): Promise<void> {
  if (existsSync(xv6Source)) return;
  await run('git', ['clone', '--depth', '1', 'https://github.com/mit-pdos/xv6-riscv.git', xv6Source]);
}

async function ensureBookletFormat(): Promise<void> {
  const formatDirectory = path.join(BOOK_SOURCE_DIR, 'xv6-riscv-src-booklet/fmt');
  if (existsSync(formatDirectory)) return;
  const bookletDirectory = path.join(BOOK_SOURCE_DIR, 'xv6-riscv-src-booklet');
  await run('./runoff', [], { cwd: bookletDirectory, tolerateFailure: true });
  if (!existsSync(formatDirectory)) {
    throw new Error('The official xv6 booklet formatter did not create fmt/. Install its documented tools and retry.');
  }
}

async function generateLatexOutput(): Promise<void> {
  await mkdir(latexOutput, { recursive: true });
  const texFiles = (await readdir(BOOK_SOURCE_DIR)).filter((file) => file.endsWith('.tex'));
  const lineref = path.join(BOOK_SOURCE_DIR, 'lineref');
  const formatDirectory = path.join(BOOK_SOURCE_DIR, 'xv6-riscv-src-booklet/fmt');
  for (const file of texFiles) {
    const outputPath = path.join(latexOutput, file);
    const { stdout } = await execFileAsync('python3', [lineref, file, xv6Source, formatDirectory], {
      cwd: BOOK_SOURCE_DIR,
      maxBuffer: 16 * 1024 * 1024
    });
    await writeFile(outputPath, stdout);
  }
}

async function runOfficialConverter(): Promise<void> {
  const converter = path.join(BOOK_SOURCE_DIR, 'convert-quarto.py');
  await rm(quartoOutput, { recursive: true, force: true });
  await run('python3', [converter]);
}

function escapeYaml(value: string): string {
  return JSON.stringify(value);
}

function markdownTitle(markdown: string, fallback: string): string {
  const heading = markdown.match(/^#\s+(.+)$/m)?.[1] ?? fallback;
  return heading.replace(/\s*\{#[^}]+\}\s*$/, '').replace(/^Chapter\s+\d+\s*[:.]?\s*/i, '').trim();
}

function figureHtml(alt: string, src: string, attributes: string): string {
  const id = attributes.match(/#([\w:.-]+)/)?.[1];
  const safeAlt = alt.replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const safeSrc = src.replace(/"/g, '&quot;');
  return `<figure${id ? ` id="${id}"` : ''}><img src="/book/${safeSrc}" alt="${safeAlt}" loading="lazy"><figcaption>${safeAlt}</figcaption></figure>`;
}

function normalizeQuarto(markdown: string, source: string): string {
  let body = markdown.replace(/\r\n/g, '\n');
  const qmdToPath = new Map<string, string>();
  for (const chapter of BOOK_CHAPTERS) {
    const qmd = chapter.source === 'acks' ? 'index' : chapter.source;
    qmdToPath.set(`${qmd}.html`, chapterPath(chapter.chapter));
  }

  body = body.replace(/^#\s+.+\n+/, '');
  body = body.replace(/^:{4,}\s+\{#refs[^}]*\}\s*$/gm, '## References');
  body = body.replace(/^:::\s+\{#(ref-[^\s}]+)[^}]*\}\s*$/gm, '<span id="$1"></span>');
  body = body.replace(/^:{3,}\s*$/gm, '');
  body = body.replace(/^(#{2,6}\s+.*?)(?:\s*\{#([^}]+)\})\s*$/gm, (_match, heading, id) => `<a id="${id}"></a>\n\n${heading}`);
  body = body.replace(/!\[([^\]]*)\]\((fig\/[^)]+)\)\{([^}]*)\}/g, (_match, alt, src, attrs) => figureHtml(alt, src, attrs));
  body = body.replace(/!\[([^\]]*)\]\((fig\/[^)]+)\)/g, (_match, alt, src) => figureHtml(alt, src, ''));
  body = body.replace(/<img\s+([^>]*?)src="fig\/([^"?#]+)"([^>]*)\/?\s*>/g, (_match, before, file, after) => {
    const attributes = `${before}src="/book/fig/${file}"${after}`.replace(/\s*\/?\s*$/, '');
    const alt = /\balt=/.test(attributes) ? '' : ' alt=""';
    const loading = /\bloading=/.test(attributes) ? '' : ' loading="lazy"';
    return `<img ${attributes}${alt}${loading}>`;
  });
  body = body.replace(/\]\(([^)]+)\)\{[^}]*\}/g, ']($1)');
  body = body.replace(/\]\{[^}\n]*\}/g, ']');
  body = body.replace(/\]\(xv6-src-booklet\.pdf\)/g, '](https://pdos.csail.mit.edu/6.1810/2026/xv6/xv6-src-booklet.pdf)');
  body = body.replace(/\]\((index|acks|unix|first|mem|trap|pgfault|interrupt|lock|sched|sleep|fs|log|lock2|sum)\.html(#[^)]+)?\)/g, (_match, page, hash = '') => {
    const htmlName = page === 'acks' ? 'index.html' : `${page}.html`;
    return `](${qmdToPath.get(htmlName) ?? htmlName}${hash})`;
  });
  body = body.replace(/```\s*\{([^}]*)\}/g, (_match, attrs) => {
    const language = /language-([\w-]+)/.exec(attrs)?.[1] ?? (/\.c(?:\s|$)/.test(attrs) ? 'c' : 'c');
    return `\`\`\`${language}`;
  });

  const remainingLatex = [...body.matchAll(/\\(begin|end|input|includegraphics|ref|cite|texttt|emph)\b/g)];
  if (remainingLatex.length > 0) {
    body = `> TODO: unsupported source construct retained from ${source}.\n\n${body}`;
  }
  return body.trim();
}

function markdownStats(markdown: string): BookMarkdownStats {
  const lines = markdown.replace(/^---\n[\s\S]*?\n---\n/, '').split('\n');
  let fenced = false;
  let codeBlocks = 0;
  let indentedRun = false;
  for (const line of lines) {
    if (/^```/.test(line)) {
      if (!fenced) codeBlocks += 1;
      fenced = !fenced;
      indentedRun = false;
      continue;
    }
    if (fenced) continue;
    if (/^( {4}|\t)\S/.test(line)) {
      if (!indentedRun) codeBlocks += 1;
      indentedRun = true;
    } else if (line.trim()) {
      indentedRun = false;
    }
  }
  return {
    headings: (markdown.match(/^#{2,6}\s+/gm) ?? []).length,
    codeBlocks,
    figures: (markdown.match(/<figure\b|!\[[^\]]*\]\(/g) ?? []).length,
    tables: (markdown.match(/<table\b/g) ?? []).length,
    footnotes: (markdown.match(/^\[\^[^\]]+\]:/gm) ?? []).length
  };
}

function assertBookContentPreserved(source: BookMarkdownStats, output: BookMarkdownStats, file: string): void {
  const protectedKeys: (keyof BookMarkdownStats)[] = ['headings', 'codeBlocks', 'figures', 'tables', 'footnotes'];
  const losses = protectedKeys.filter((key) => output[key] < source[key]);
  if (losses.length > 0) {
    throw new Error(`${file}: Book content-loss guard failed (${losses.map((key) => `${key}: ${source[key]} -> ${output[key]}`).join(', ')})`);
  }
}

async function copyFigures(): Promise<void> {
  await rm(BOOK_FIGURE_DIR, { recursive: true, force: true });
  await mkdir(BOOK_FIGURE_DIR, { recursive: true });
  const figureSource = path.join(BOOK_SOURCE_DIR, 'fig');
  for (const file of await readdir(figureSource)) {
    if (/\.(svg|png)$/i.test(file)) await cp(path.join(figureSource, file), path.join(BOOK_FIGURE_DIR, file));
  }
}

async function fetchGeneratedFigures(): Promise<void> {
  for (const file of generatedFigures) {
    const output = path.join(BOOK_FIGURE_DIR, file);
    if (existsSync(output)) continue;
    const source = new URL(`fig/${file}`, publishedBookBase);
    const response = await fetch(source, {
      headers: { 'User-Agent': 'xv6-local-reader/1.0' },
      signal: AbortSignal.timeout(30_000)
    });
    if (!response.ok) throw new Error(`Unable to fetch official generated figure ${source.href}: ${response.status}`);
    await writeFile(output, new Uint8Array(await response.arrayBuffer()));
    console.log(`${file}: fetched from the official published build`);
  }
}

async function applyCitations(qmdPath: string): Promise<string> {
  const { stdout } = await execFileAsync('quarto', [
    'pandoc', qmdPath,
    '-f', 'markdown',
    '-t', 'markdown-raw_attribute',
    '--wrap=none',
    '--citeproc',
    `--bibliography=${path.join(BOOK_SOURCE_DIR, 'book.bib')}`
  ], {
    cwd: BOOK_SOURCE_DIR,
    maxBuffer: 32 * 1024 * 1024
  });
  return stdout;
}

async function convertMarkdown(commit: string): Promise<void> {
  await rm(BOOK_CONTENT_DIR, { recursive: true, force: true });
  await mkdir(BOOK_CONTENT_DIR, { recursive: true });
  const conversionStats: BookConversionRecord[] = [];
  for (const chapter of BOOK_CHAPTERS) {
    const qmdName = chapter.source === 'acks' ? 'index.qmd' : `${chapter.source}.qmd`;
    const qmdPath = path.join(quartoOutput, qmdName);
    const raw = await applyCitations(qmdPath);
    const title = markdownTitle(raw, chapter.slug);
    const body = normalizeQuarto(raw, qmdName);
    const fileName = `${String(chapter.chapter).padStart(2, '0')}-${chapter.slug}.md`;
    const frontmatter = [
      '---',
      `title: ${escapeYaml(title)}`,
      `chapter: ${chapter.chapter}`,
      'source: "xv6-riscv-book"',
      `sourceFile: ${escapeYaml(`${chapter.source}.tex`)}`,
      `commit: ${escapeYaml(commit)}`,
      '---',
      ''
    ].join('\n');
    const output = `${frontmatter}${body}\n`;
    const sourceStats = markdownStats(raw);
    const outputStats = markdownStats(output);
    assertBookContentPreserved(sourceStats, outputStats, fileName);
    conversionStats.push({ chapter: chapter.chapter, file: fileName, source: sourceStats, output: outputStats });
    await writeFile(path.join(BOOK_CONTENT_DIR, fileName), output);
    console.log(`${fileName}: generated`);
  }
  await mkdir(path.dirname(BOOK_CONVERSION_STATS_PATH), { recursive: true });
  await writeFile(BOOK_CONVERSION_STATS_PATH, `${JSON.stringify({ generatedAt: new Date().toISOString(), chapters: conversionStats }, null, 2)}\n`);
}

async function sync(): Promise<void> {
  await ensureBookRepository();
  const commit = await sourceCommit();
  await ensureXv6Source();
  await ensureBookletFormat();
  await generateLatexOutput();
  await runOfficialConverter();
  await copyFigures();
  await fetchGeneratedFigures();
  await convertMarkdown(commit);
  await mkdir(path.dirname(BOOK_VERSION_PATH), { recursive: true });
  await writeFile(BOOK_VERSION_PATH, `${JSON.stringify({
    repository: BOOK_REPOSITORY.replace(/\.git$/, ''),
    commit,
    fetchedAt: new Date().toISOString(),
    converter: 'official convert-quarto.py, followed by Quarto citeproc',
    generatedFigureSource: publishedBookBase,
    generatedFigures
  }, null, 2)}\n`);
  console.log(`book: ${BOOK_CHAPTERS.length} Markdown files generated from ${commit.slice(0, 12)}`);
}

await sync();
