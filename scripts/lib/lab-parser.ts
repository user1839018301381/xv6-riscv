import { load } from 'cheerio';
import hljs from 'highlight.js/lib/core';
import bash from 'highlight.js/lib/languages/bash';
import c from 'highlight.js/lib/languages/c';
import makefile from 'highlight.js/lib/languages/makefile';
import plaintext from 'highlight.js/lib/languages/plaintext';
import x86asm from 'highlight.js/lib/languages/x86asm';
import { chapterPath, slugifyHeading } from './config.ts';
import type { ContentStats, OutlineItem, ProcessedLab } from './types.ts';

hljs.registerLanguage('bash', bash);
hljs.registerLanguage('c', c);
hljs.registerLanguage('makefile', makefile);
hljs.registerLanguage('plaintext', plaintext);
hljs.registerLanguage('x86asm', x86asm);

const START_MARKER = '<!-- START SECTION: body -->';
const END_MARKER = '<!-- END SECTION: body -->';

export function extractLabBody(rawHtml: string): string {
  const start = rawHtml.indexOf(START_MARKER);
  const end = rawHtml.indexOf(END_MARKER);
  if (start < 0 || end < 0 || end <= start) {
    throw new Error('MIT Lab HTML did not contain the expected body boundary comments. Raw HTML was preserved; parser stopped.');
  }
  return rawHtml.slice(start + START_MARKER.length, end);
}

function collectStats(html: string): ContentStats {
  const $ = load(`<main>${html}</main>`, null, false);
  return {
    h1: $('h1').length,
    h2: $('h2').length,
    h3: $('h3').length,
    paragraphs: $('p').length,
    codeBlocks: $('pre').length,
    links: $('a[href]').length,
    lists: $('ul, ol').length,
    tables: $('table').length,
    figures: $('figure, img').length
  };
}

function detectLanguage(code: string): 'bash' | 'c' | 'makefile' | 'x86asm' | 'plaintext' {
  const sample = code.trim();
  if (/^(\$ |# |git |make(?:\s|$)|qemu|grep |echo |cd )/m.test(sample)) return 'bash';
  if (/^(\.globl|\.section|[a-z][\w.]*:\s*$)|\b(a[0-7]|sp|ra|ecall|sret)\b/m.test(sample)) return 'x86asm';
  if (/^[A-Z_][A-Z0-9_]*\s*[:?+]?=|^\w[\w./-]*:\s/m.test(sample)) return 'makefile';
  if (/#include|\b(struct|void|int|uint64|char|return|sizeof)\b|->|\{\s*$/m.test(sample)) return 'c';
  return 'plaintext';
}

function assertNoLoss(before: ContentStats, after: ContentStats): void {
  const protectedKeys: (keyof ContentStats)[] = [
    'h1', 'h2', 'h3', 'paragraphs', 'codeBlocks', 'links', 'lists', 'tables', 'figures'
  ];
  const losses = protectedKeys.filter((key) => after[key] < before[key]);
  if (losses.length > 0) {
    const details = losses.map((key) => `${key}: ${before[key]} -> ${after[key]}`).join(', ');
    throw new Error(`Content-loss guard failed (${details}). Raw HTML was left untouched.`);
  }
}

export function parseLab(rawHtml: string, options: {
  slug: string;
  source: string;
  fetchedAt: string;
  assetMap?: Map<string, string>;
}): ProcessedLab {
  const body = extractLabBody(rawHtml);
  const sourceStats = collectStats(body);
  const $ = load(`<main id="lab-source">${body}</main>`, null, false);
  const root = $('#lab-source');

  root.find('script, style').remove();

  const idCounts = new Map<string, number>();
  const outline: OutlineItem[] = [];
  let section = 0;
  let step = 0;

  root.find('h1, h2, h3').each((_, element) => {
    const heading = $(element);
    const text = heading.text().replace(/\s+/g, ' ').trim();
    let id = heading.attr('id') || slugifyHeading(text);
    const count = idCounts.get(id) ?? 0;
    idCounts.set(id, count + 1);
    if (count > 0) id = `${id}-${count + 1}`;
    heading.attr('id', id);

    if (element.tagName === 'h2') {
      section += 1;
      step = 0;
      const number = String(section).padStart(2, '0');
      heading.attr('data-section-number', number);
      outline.push({ depth: 2, id, text, number });
    } else if (element.tagName === 'h3') {
      if (section === 0) section = 1;
      step += 1;
      const number = `${String(section).padStart(2, '0')}.${step}`;
      heading.attr('data-section-number', number);
      outline.push({ depth: 3, id, text, number });
    }
  });

  root.find('a[href]').each((_, element) => {
    const anchor = $(element);
    const href = anchor.attr('href');
    if (!href || href.startsWith('#') || href.startsWith('mailto:')) return;

    const isOfficialBook = /mit-pdos\.github\.io\/xv6-riscv-book/.test(href);
    const linkChapter = anchor.text().match(/Chapter\s+(\d+)/i);
    const contextChapter = anchor.parent().text().match(/Chapter\s+(\d+)/i);
    const chapterMatch = linkChapter ?? contextChapter;
    if (isOfficialBook && chapterMatch) {
      const chapter = Number(chapterMatch[1]);
      if (chapter >= 0 && chapter <= 13) {
        anchor.attr('href', chapterPath(chapter));
        anchor.addClass('book-link');
        return;
      }
    }

    try {
      const absolute = new URL(href, options.source);
      anchor.attr('href', absolute.href);
      if (absolute.origin !== new URL(options.source).origin) {
        anchor.addClass('external-link');
        anchor.attr('rel', 'noreferrer');
      }
    } catch {
      anchor.addClass('unresolved-link');
    }
  });

  root.find('img[src]').each((_, element) => {
    const image = $(element);
    const src = image.attr('src');
    if (!src) return;
    const absolute = new URL(src, options.source).href;
    image.attr('src', options.assetMap?.get(absolute) ?? absolute);
    image.attr('loading', 'lazy');
    if (!image.attr('alt')) image.attr('alt', '');
  });

  root.find('[class]').each((_, element) => {
    const className = $(element).attr('class') ?? '';
    if (/(^|\s)hint(?:\s|$)/i.test(className)) $(element).addClass('callout-hint');
  });

  root.find('pre').each((_, element) => {
    const pre = $(element);
    const code = pre.text().replace(/\n$/, '');
    const language = detectLanguage(code);
    const highlighted = language === 'plaintext'
      ? hljs.highlight(code, { language: 'plaintext' }).value
      : hljs.highlight(code, { language }).value;
    pre.attr('data-language', language);
    pre.attr('tabindex', '0');
    pre.html(`<code class="hljs language-${language}">${highlighted}</code>`);
  });

  const html = root.html() ?? '';
  const renderedStats = collectStats(html);
  assertNoLoss(sourceStats, renderedStats);

  const title = root.find('h1').first().text().replace(/\s+/g, ' ').trim();
  if (!title) throw new Error(`Lab ${options.slug} has no h1 after parsing.`);

  return {
    slug: options.slug,
    title,
    source: options.source,
    fetchedAt: options.fetchedAt,
    outline,
    html,
    sourceStats,
    renderedStats
  };
}

export function labAssetUrls(rawHtml: string, source: string): string[] {
  const body = extractLabBody(rawHtml);
  const $ = load(`<main>${body}</main>`, null, false);
  const urls = new Set<string>();
  $('img[src]').each((_, element) => {
    const src = $(element).attr('src');
    if (!src) return;
    const absolute = new URL(src, source);
    if (absolute.origin === new URL(source).origin) urls.add(absolute.href);
  });
  return [...urls];
}
