/**
 * Generate the Japanese reading copy from the cached official material.
 *
 * This is intentionally an offline-site build step: Google Translate is used
 * only while generating the files, and the finished site has no translation
 * runtime or network dependency. Markdown/HTML structure, links, identifiers,
 * code, and command output are protected; comments in C/assembly snippets are
 * translated separately.
 *
 * Usage (with a running agent-browser Google Translate tab):
 *   AGENT_BROWSER_SESSION=... npm run translate:ja
 */

import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { load } from 'cheerio';
import {
  BOOK_CONTENT_DIR,
  LAB_PROCESSED_DIR,
  LABS,
  PROJECT_ROOT
} from './lib/config.ts';

const session = process.env.AGENT_BROWSER_SESSION;
if (!session) throw new Error('AGENT_BROWSER_SESSION is required. Open Google Translate in the named agent-browser session first.');

const BOOK_JA_DIR = path.join(PROJECT_ROOT, 'content/book-ja');
const LAB_JA_DIR = path.join(PROJECT_ROOT, 'data/labs/processed-ja');
const BREAK = 'XV6_NODE_BREAK_9F3A';
const MAX_CHARS = 4_200;
const COMMENT_LANGUAGES = /^(c|cpp|c\+\+|asm|x86asm|riscv|risc-v)$/i;
const ENGLISH_MARKERS = /\b(the|this|that|these|those|please|read|run|implement|when|where|which|what|your|some|for|from|with|without|chapter|code|real world|exercises|references|the allocator|the kernel|the process|a process|an? )\b/i;

interface TranslationUnit {
  text: string;
  apply(value: string): void;
}

function looksLikeEnglish(value: string): boolean {
  const words = value.match(/[A-Za-z]{3,}/g) ?? [];
  return words.length >= 2 && ENGLISH_MARKERS.test(value) && !/^https?:\/\//i.test(value.trim());
}

function splitMarkdownSegments(body: string): string[] {
  const segments: string[] = [];
  let current: string[] = [];
  let inFence = false;
  for (const line of body.split('\n')) {
    if (/^\s*```/.test(line)) inFence = !inFence;
    if (!inFence && line.trim() === '') {
      if (current.length > 0) {
        segments.push(current.join('\n'));
        current = [];
      }
      continue;
    }
    current.push(line);
  }
  if (current.length > 0) segments.push(current.join('\n'));
  return segments;
}

function batches(units: TranslationUnit[]): { units: TranslationUnit[]; text: string }[] {
  const result: { units: TranslationUnit[]; text: string }[] = [];
  let current: TranslationUnit[] = [];
  let length = 0;
  for (const unit of units) {
    const extra = current.length === 0 ? unit.text.length : unit.text.length + BREAK.length + 4;
    if (current.length > 0 && length + extra > MAX_CHARS) {
      result.push({ units: current, text: current.map((item) => item.text).join(`\n\n${BREAK}\n\n`) });
      current = [];
      length = 0;
    }
    current.push(unit);
    length += current.length === 1 ? unit.text.length : extra;
  }
  if (current.length > 0) result.push({ units: current, text: current.map((item) => item.text).join(`\n\n${BREAK}\n\n`) });
  return result;
}

function parseEvalOutput(stdout: string): string[] {
  const start = stdout.indexOf('[');
  const end = stdout.lastIndexOf(']');
  if (start < 0 || end < start) throw new Error(`Unexpected agent-browser output: ${stdout.slice(-500)}`);
  const parsed = JSON.parse(stdout.slice(start, end + 1)) as unknown;
  if (!Array.isArray(parsed) || !parsed.every((item) => typeof item === 'string')) throw new Error('Translation result was not a string array.');
  return parsed;
}

async function translateInBrowser(texts: string[]): Promise<string[]> {
  const encoded = Buffer.from(JSON.stringify(texts), 'utf8').toString('base64');
  const script = `(async () => {
    const bytes = Uint8Array.from(atob(${JSON.stringify(encoded)}), (char) => char.charCodeAt(0));
    const chunks = JSON.parse(new TextDecoder().decode(bytes));
    const input = document.querySelector('textarea[aria-label="Source text"]');
    const setter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value').set;
    const getOutput = () => document.querySelector('textarea.ZJtmid') || Array.from(document.querySelectorAll('textarea')).find((node) => node !== input && node.value && node.value !== input.value);
    if (!input || !setter) throw new Error('Google Translate input is not available.');
    const output = [];
    for (const chunk of chunks) {
      const previous = getOutput()?.value || '';
      setter.call(input, chunk);
      input.dispatchEvent(new Event('input', {bubbles: true}));
      input.dispatchEvent(new Event('change', {bubbles: true}));
      let translated = '';
      for (let attempt = 0; attempt < 60; attempt += 1) {
        await new Promise((resolve) => setTimeout(resolve, 250));
        const candidate = document.querySelector('textarea.ZJtmid') || Array.from(document.querySelectorAll('textarea')).find((node) => node !== input && node.value && node.value !== input.value);
        // Long Markdown batches can take several seconds to replace the
        // previous result. Waiting longer here avoids accepting a stale
        // translation that still contains another segment's placeholders.
        if (candidate?.value && (candidate.value !== previous || attempt >= 40)) { translated = candidate.value; break; }
      }
      if (!translated) throw new Error('Google Translate did not return a result.');
      output.push(translated);
    }
    return output;
  })()`;
  for (let attempt = 1; attempt <= 3; attempt += 1) {
    try {
      const stdout = await new Promise<string>((resolve, reject) => {
        const child = spawn('agent-browser', ['--session', session!, 'eval', '--stdin', '--timeout', '600000'], {
          stdio: ['pipe', 'pipe', 'pipe']
        });
        let output = '';
        let errorOutput = '';
        child.stdout.on('data', (chunk) => { output += chunk.toString(); });
        child.stderr.on('data', (chunk) => { errorOutput += chunk.toString(); });
        child.on('error', reject);
        child.on('close', (code) => code === 0 ? resolve(output) : reject(new Error(errorOutput || `agent-browser exited with status ${code}`)));
        child.stdin.end(script);
      });
      return parseEvalOutput(stdout);
    } catch (error) {
      if (attempt === 3) throw error;
      await new Promise((resolve) => setTimeout(resolve, 2_000));
    }
  }
  throw new Error('Translation failed.');
}

async function translateUnits(units: TranslationUnit[]): Promise<void> {
  for (const batch of batches(units)) {
    const signature = (value: string): string[] => [...value.matchAll(/ZQX9[KC]\s*(\d+)\s*Z/giu)]
      .map((match) => match[0].replace(/\s+/g, '').toUpperCase())
      .sort();
    const valid = (source: string, translated: string): boolean => {
      const expected = signature(source);
      const actual = signature(translated);
      // Google can occasionally reorder a placeholder or emit one duplicate
      // while preserving the surrounding sentence. Require every expected
      // token and reject unknown ones, but tolerate harmless duplicates; the
      // restore step removes duplicates before putting the original text back.
      const expectedSet = new Set(expected);
      return expected.every((token) => actual.includes(token)) && actual.every((token) => expectedSet.has(token));
    };
    let parts: string[] | undefined;
    for (let attempt = 1; attempt <= 3; attempt += 1) {
      const translated = await translateInBrowser([batch.text]);
      const candidate = translated[0].split(BREAK);
      if (candidate.length === batch.units.length && candidate.every((part, index) => valid(batch.units[index].text, part))) {
        parts = candidate;
        break;
      }
      console.warn(`translation batch did not preserve placeholders (attempt ${attempt}); retrying`);
    }
    if (!parts) {
      // Short residual paragraphs can make Google drop the separator. Retry
      // those few units individually instead of risking a misaligned rewrite.
      const individual: string[] = [];
      for (const unit of batch.units) {
        let translatedUnit: string | undefined;
        for (let attempt = 1; attempt <= 3; attempt += 1) {
          const [candidate] = await translateInBrowser([unit.text]);
          if (candidate !== undefined && valid(unit.text, candidate)) {
            translatedUnit = candidate;
            break;
          }
          console.warn(`individual translation did not preserve placeholders (attempt ${attempt}); retrying`);
        }
        if (translatedUnit === undefined) throw new Error('Translation did not preserve protected structure for an individual segment.');
        individual.push(translatedUnit);
      }
      individual.forEach((part, index) => batch.units[index].apply(part.trim()));
      console.log(`translated ${batch.units.length} residual segments individually`);
      continue;
    }
    parts.forEach((part, index) => batch.units[index].apply(part.trim()));
    console.log(`translated ${batch.units.length} segments (${batch.text.length} characters)`);
  }
}

function protectMarkdown(value: string): { masked: string; restore(value: string): string } {
  const slots: string[] = [];
  const put = (match: string): string => {
    const token = `ZQX9K${slots.length}Z`;
    slots.push(match);
    return token;
  };
  let masked = value;
  masked = masked.replace(/`+[^`\n]+`+/g, put);
  masked = masked.replace(/(\]\()([^)]*)(\))/g, (_match, open: string, target: string, close: string) => `${open}${put(target)}${close}`);
  masked = masked.replace(/<https?:\/\/[^>]+>/gi, put);
  masked = masked.replace(/<[^>]+>/g, put);
  masked = masked.replace(/\$\$[\s\S]*?\$\$|\$[^$\n]+\$/g, put);
  masked = masked.replace(/\{[^}\n]+\}/g, put);
  masked = masked.replace(/\[\^[^\]]+\]/g, put);
  // Some source material (notably the API and source-file tables) contains
  // code outside Markdown backticks. Keep those signatures and identifiers
  // intact so the translator cannot turn `fork` into a Japanese word.
  masked = masked.replace(/\b(?:void|char|short|long|unsigned|int|uint(?:8|16|32|64)?|size_t|ssize_t|struct\s+[A-Za-z_]\w*|enum\s+[A-Za-z_]\w*)(?:\s+(?:\\)?\*+)?\s*[A-Za-z_]\w*\s*\([^\n)]*\)/g, put);
  masked = masked.replace(/\b[A-Za-z_]\w*'s\s+[A-Za-z_]\w*\s*-?\\?>\s*[A-Za-z_]\w*/g, put);
  masked = masked.replace(/\b[A-Za-z_]\w*\s*(?:->|\\->)\s*[A-Za-z_]\w*/g, put);
  masked = masked.replace(/\b[A-Za-z_]\w*\.[A-Za-z_]\w+\b/g, put);
  masked = masked.replace(/\b[A-Za-z_]\w*\s*\([^\n)]*\)/g, put);
  masked = masked.replace(/\b[A-Za-z_]\w*_[A-Za-z0-9_]+\b/g, put);
  return {
    masked,
    restore(translated: string): string {
      const seen = new Set<number>();
      return translated.replace(/ZQX9K\s*(\d+)\s*Z/gi, (match, index: string) => {
        const slot = Number(index);
        if (seen.has(slot)) return '';
        seen.add(slot);
        return slots[slot] ?? match;
      });
    }
  };
}

async function translateCodeComments(block: string): Promise<string> {
  const fence = block.match(/^```([^\n]*)/);
  const language = fence?.[1]?.trim().split(/[\s{]/)[0] ?? '';
  if (!COMMENT_LANGUAGES.test(language)) return block;
  const comments: { token: string; text: string }[] = [];
  let masked = block;
  const put = (match: string): string => {
    const token = `ZQX9C${comments.length}Z`;
    comments.push({ token, text: match });
    return token;
  };
  masked = masked.replace(/\/\/[^\n]*/g, put);
  masked = masked.replace(/\/\*[\s\S]*?\*\//g, put);
  if (/asm|risc/i.test(language)) masked = masked.replace(/(^|\n)(\s*#(?!include|define|if|ifdef|ifndef|endif|else|elif|error|pragma)[^\n]*)/g, (_match, prefix: string, comment: string) => `${prefix}${put(comment)}`);
  if (comments.length === 0) return block;
  const units = comments.map((comment) => ({
    text: comment.text,
    apply: (value: string) => { comment.text = value; }
  }));
  await translateUnits(units);
  for (const comment of comments) masked = masked.replace(comment.token, comment.text);
  return masked;
}

function isIndentedCodeSegment(segment: string): boolean {
  const lines = segment.split('\n');
  return lines.length > 0 && lines.every((line) => !line.trim() || /^(?: {4}|\t)/.test(line));
}

async function translateIndentedCodeComments(block: string): Promise<string> {
  const comments: { token: string; text: string }[] = [];
  let masked = block;
  const put = (match: string): string => {
    const token = `ZQX9C${comments.length}Z`;
    comments.push({ token, text: match });
    return token;
  };
  masked = masked.replace(/\/\/[^\n]*/g, put);
  masked = masked.replace(/\/\*[\s\S]*?\*\//g, put);
  masked = masked.replace(/(^|\n)(\s*#(?!include|define|if|ifdef|ifndef|endif|else|elif|error|pragma)[^\n]*)/g, (_match, prefix: string, comment: string) => `${prefix}${put(comment)}`);
  if (comments.length === 0) return block;
  await translateUnits(comments.map((comment) => ({
    text: comment.text,
    apply: (value: string) => { comment.text = value; }
  })));
  for (const comment of comments) masked = masked.replace(comment.token, comment.text);
  return masked;
}

async function translateBookFile(markdown: string): Promise<string> {
  const match = markdown.match(/^---\n([\s\S]*?)\n---\n([\s\S]*)$/);
  if (!match) throw new Error('Book file is missing frontmatter.');
  const frontmatter = match[1];
  const body = match[2];
  const titleMatch = frontmatter.match(/^title:\s*(.+)$/m);
  let translatedFrontmatter = frontmatter;
  if (titleMatch) {
    const titleValue = titleMatch[1].replace(/^"|"$/g, '');
    let translatedTitle = '';
    await translateUnits([{ text: titleValue, apply: (value) => { translatedTitle = value; } }]);
    translatedFrontmatter = frontmatter.replace(titleMatch[1], JSON.stringify(translatedTitle));
  }
  const segments = splitMarkdownSegments(body);
  const output: string[] = [];
  const segmentUnits: TranslationUnit[] = [];
  const segmentValues = new Map<number, string>();
  for (const segment of segments) {
    if (!segment.trim()) { output.push(segment); continue; }
    if (/^```/.test(segment.trim())) {
      output.push(await translateCodeComments(segment));
      continue;
    }
    if (isIndentedCodeSegment(segment)) {
      output.push(await translateIndentedCodeComments(segment));
      continue;
    }
    if (/^(<a id=|<img |\{#|:{3,})/.test(segment.trim()) && !/<figcaption>|<p>[^<]/.test(segment)) {
      output.push(segment);
      continue;
    }
    const protectedSegment = protectMarkdown(segment);
    const index = output.length;
    output.push('');
    segmentUnits.push({ text: protectedSegment.masked, apply: (value) => {
      const restored = cleanJapanese(protectedSegment.restore(value));
      if (/ZQX9K\s*\d+\s*Z/iu.test(restored)) console.warn(`protected token remained in book segment ${index}:`, restored.slice(0, 500));
      segmentValues.set(index, restored);
    } });
  }
  await translateUnits(segmentUnits);
  for (const [index, value] of segmentValues) output[index] = value;
  const translatedMarkdown = `---\n${translatedFrontmatter}\n---\n${output.join('\n\n')}`;
  const leftover = translatedMarkdown.match(/ZQX9K\s*\d+\s*Z|ZQX9C\s*\d+\s*Z/giu);
  if (leftover) throw new Error(`Book translation left a protected token behind: ${leftover.join(', ')}`);
  return translatedMarkdown;
}

function cleanJapanese(value: string): string {
  return value.replace(/Chapter(?:Â|\u00a0)?\s*(?=\[)/g, '第');
}

async function translateLabFile(input: string): Promise<string> {
  const source = JSON.parse(input) as { slug: string; title: string; source: string; fetchedAt: string; html: string; sourceStats: unknown; renderedStats: unknown };
  const $ = load(`<div id="xv6-body">${source.html}</div>`, null, false);
  const body = $('#xv6-body');
  const units: TranslationUnit[] = [];
  const visit = (node: any, blocked = false): void => {
    if (node.type === 'text') {
      if (blocked) return;
      const raw = node.data ?? '';
      const value = raw.trim();
      if (!value) return;
      // Lab HTML has a few code/API names outside <tt>/<code> (for example
      // headings such as `sleep` and prose containing `fork()`). Reuse the
      // Markdown protection pass so those tokens stay in their source form.
      const parent = node.parent?.name?.toLowerCase?.() ?? '';
      const headingCode = ['sleep', 'sixfive', 'memdump', 'find', 'exec'].includes(value.toLowerCase()) && ['h2', 'h3'].includes(parent);
      if (headingCode) return;
      const protectedText = protectMarkdown(value);
      units.push({ text: protectedText.masked, apply: (translated) => {
        node.data = raw.replace(value, protectedText.restore(translated));
      } });
      return;
    }
    const name = typeof node.name === 'string' ? node.name.toLowerCase() : '';
    const nextBlocked = blocked || name === 'script' || name === 'style' || name === 'pre' || name === 'code' || name === 'tt' || name === 'kbd';
    for (const child of node.children ?? []) visit(child, nextBlocked);
  };
  if (body[0]) visit(body[0]);
  await translateUnits(units);

  const commentUnits: TranslationUnit[] = [];
  body.find('pre[data-language] code').each((_index, element) => {
    const language = $(element).parent('pre').attr('data-language') ?? '';
    if (!COMMENT_LANGUAGES.test(language)) return;
    $(element).find('.hljs-comment').each((_commentIndex, comment) => {
      const raw = $(comment).text();
      const value = raw.trim();
      if (!value) return;
      commentUnits.push({ text: value, apply: (translated) => $(comment).text(raw.replace(value, translated)) });
    });
  });
  await translateUnits(commentUnits);

  /*
   * The translated h1/outline are derived from the DOM after all text nodes
   * have been replaced, so the page's navigation follows the Japanese copy.
   */
  const title = body.find('h1').first().text().trim() || source.title;
  const outline = body.find('h2, h3').toArray().map((heading) => {
    const current = $(heading);
    const depth = heading.tagName.toLowerCase() === 'h2' ? 2 : 3;
    return { depth, id: current.attr('id') ?? '', text: current.text().trim(), number: current.attr('data-section-number') ?? '' };
  });
  const translatedHtml = body.html() ?? '';
  if (/ZQX9K\d+Z|ZQX9C\d+Z/u.test(translatedHtml)) throw new Error('Lab translation left a protected token behind.');
  return `${JSON.stringify({ ...source, title, html: translatedHtml, outline, renderedStats: source.renderedStats }, null, 2)}\n`;
}

async function translateBookResidual(markdown: string): Promise<string> {
  const match = markdown.match(/^---\n([\s\S]*?)\n---\n([\s\S]*)$/);
  if (!match) return markdown;
  const segments = splitMarkdownSegments(match[2]);
  const units: TranslationUnit[] = [];
  const values = new Map<number, string>();
  const output: string[] = [];
  for (const segment of segments) {
    const index = output.length;
    output.push(segment);
    if (!segment.trim() || /^```/.test(segment.trim()) || !looksLikeEnglish(segment)) continue;
    const protectedSegment = protectMarkdown(segment);
    units.push({ text: protectedSegment.masked, apply: (value) => values.set(index, cleanJapanese(protectedSegment.restore(value))) });
  }
  await translateUnits(units);
  for (const [index, value] of values) output[index] = value;
  return `---\n${match[1]}\n---\n${output.join('\n\n')}`;
}

async function translateLabResidual(input: string): Promise<string> {
  const source = JSON.parse(input) as { title: string; html: string; outline: unknown; [key: string]: unknown };
  const $ = load(`<div id="xv6-body">${source.html}</div>`, null, false);
  const body = $('#xv6-body');
  const units: TranslationUnit[] = [];
  const visit = (node: any, blocked = false): void => {
    if (node.type === 'text') {
      if (blocked) return;
      const raw = node.data ?? '';
      const value = raw.trim();
      if (!value || !looksLikeEnglish(value)) return;
      const protectedText = protectMarkdown(value);
      units.push({ text: protectedText.masked, apply: (translated) => {
        node.data = raw.replace(value, protectedText.restore(translated));
      } });
      return;
    }
    const name = typeof node.name === 'string' ? node.name.toLowerCase() : '';
    const nextBlocked = blocked || name === 'script' || name === 'style' || name === 'pre' || name === 'code' || name === 'tt' || name === 'kbd';
    for (const child of node.children ?? []) visit(child, nextBlocked);
  };
  if (body[0]) visit(body[0]);
  await translateUnits(units);
  const title = body.find('h1').first().text().trim() || source.title;
  const outline = body.find('h2, h3').toArray().map((heading) => {
    const current = $(heading);
    return {
      depth: heading.tagName.toLowerCase() === 'h2' ? 2 : 3,
      id: current.attr('id') ?? '',
      text: current.text().trim(),
      number: current.attr('data-section-number') ?? ''
    };
  });
  return JSON.stringify({ ...source, title, outline, html: body.html() ?? '' }, null, 2) + '\n';
}

async function main(): Promise<void> {
  const only = process.env.TRANSLATE_ONLY;
  const bookFiles = (await readdir(BOOK_CONTENT_DIR)).filter((name) => name.endsWith('.md') && (!only || name.includes(only))).sort();
  const selectedLabs = LABS.filter((item) => !only || item.slug === only);
  if (process.env.RESIDUAL_ONLY !== '1') {
    if (process.env.KEEP_EXISTING !== '1') {
      await rm(BOOK_JA_DIR, { recursive: true, force: true });
      await rm(LAB_JA_DIR, { recursive: true, force: true });
    }
    await mkdir(BOOK_JA_DIR, { recursive: true });
    await mkdir(LAB_JA_DIR, { recursive: true });
    for (const file of bookFiles) {
      const translated = await translateBookFile(await readFile(path.join(BOOK_CONTENT_DIR, file), 'utf8'));
      await writeFile(path.join(BOOK_JA_DIR, file), translated);
      console.log(`book-ja/${file}: generated`);
    }

    for (const lab of selectedLabs) {
      const file = `${lab.slug}.json`;
      if (!existsSync(path.join(LAB_PROCESSED_DIR, file))) throw new Error(`Missing ${file}`);
      const translated = await translateLabFile(await readFile(path.join(LAB_PROCESSED_DIR, file), 'utf8'));
      await writeFile(path.join(LAB_JA_DIR, file), translated);
      console.log(`labs-ja/${file}: generated`);
    }
  }

  // The first pass translates every visible text unit. A residual pass is
  // opt-in because translating already mixed Japanese/Markdown can damage
  // links and inline structure; set SKIP_RESIDUAL=0 only when reviewing it.
  if (process.env.SKIP_RESIDUAL !== '0') return;
  // Google occasionally leaves a short sentence untranslated in a long batch;
  // run a focused residual pass so the visible body does not retain English.
  for (const file of bookFiles) {
    const filePath = path.join(BOOK_JA_DIR, file);
    await writeFile(filePath, await translateBookResidual(await readFile(filePath, 'utf8')));
  }
  for (const lab of selectedLabs) {
    const filePath = path.join(LAB_JA_DIR, `${lab.slug}.json`);
    await writeFile(filePath, await translateLabResidual(await readFile(filePath, 'utf8')));
  }
}

await main();
