import path from 'node:path';

export const PROJECT_ROOT = path.resolve(process.cwd());
export const LAB_BASE_URL = 'https://pdos.csail.mit.edu/6.S081/2026/labs/';
export const COURSE_SCHEDULE_URL = 'https://pdos.csail.mit.edu/6.S081/2026/schedule.html';
export const USER_AGENT = 'xv6-local-reader/1.0';
export const REQUEST_INTERVAL_MS = 3_000;

export const LABS = [
  { slug: 'util', label: 'Unix utilities' },
  { slug: 'syscall', label: 'System calls' },
  { slug: 'pgtbl', label: 'Page tables' },
  { slug: 'traps', label: 'Traps' },
  { slug: 'cow', label: 'Copy-on-write' },
  { slug: 'net', label: 'Network driver' },
  { slug: 'lock', label: 'Locks' },
  { slug: 'fs', label: 'File system' },
  { slug: 'mmap', label: 'Mmap' }
] as const;

export type LabSlug = (typeof LABS)[number]['slug'];

export const LAB_DATA_DIR = path.join(PROJECT_ROOT, 'data/labs');
export const LAB_RAW_DIR = path.join(LAB_DATA_DIR, 'raw');
export const LAB_PROCESSED_DIR = path.join(LAB_DATA_DIR, 'processed');
export const LAB_JA_PROCESSED_DIR = path.join(LAB_DATA_DIR, 'processed-ja');
export const LAB_ASSET_DIR = path.join(PROJECT_ROOT, 'public/labs-assets');
export const LAB_METADATA_PATH = path.join(LAB_DATA_DIR, 'metadata.json');
export const BOOK_SOURCE_DIR = path.join(PROJECT_ROOT, 'data/book/source');
export const BOOK_CONTENT_DIR = path.join(PROJECT_ROOT, 'content/book');
export const BOOK_JA_CONTENT_DIR = path.join(PROJECT_ROOT, 'content/book-ja');
export const BOOK_VERSION_PATH = path.join(PROJECT_ROOT, 'data/book/source-version.json');
export const BOOK_CONVERSION_STATS_PATH = path.join(PROJECT_ROOT, 'data/book/conversion-stats.json');
export const BOOK_FIGURE_DIR = path.join(PROJECT_ROOT, 'public/book/fig');

export const BOOK_REPOSITORY = 'https://github.com/mit-pdos/xv6-riscv-book.git';

export const BOOK_CHAPTERS = [
  { source: 'acks', slug: 'acknowledgments', chapter: 0 },
  { source: 'unix', slug: 'operating-system-interfaces', chapter: 1 },
  { source: 'first', slug: 'operating-system-organization', chapter: 2 },
  { source: 'mem', slug: 'page-tables', chapter: 3 },
  { source: 'trap', slug: 'traps-and-system-calls', chapter: 4 },
  { source: 'pgfault', slug: 'page-faults', chapter: 5 },
  { source: 'interrupt', slug: 'interrupts-and-device-drivers', chapter: 6 },
  { source: 'lock', slug: 'locking', chapter: 7 },
  { source: 'sched', slug: 'scheduling', chapter: 8 },
  { source: 'sleep', slug: 'sleep-and-wakeup', chapter: 9 },
  { source: 'fs', slug: 'file-system', chapter: 10 },
  { source: 'log', slug: 'crash-recovery', chapter: 11 },
  { source: 'lock2', slug: 'locking-revisited', chapter: 12 },
  { source: 'sum', slug: 'summary', chapter: 13 }
] as const;

export function chapterPath(chapter: number): string {
  return `/book/chapter-${chapter}/`;
}

export function slugifyHeading(input: string): string {
  const normalized = input
    .normalize('NFKD')
    .toLowerCase()
    .replace(/[^\p{L}\p{N}\s-]/gu, '')
    .trim()
    .replace(/[\s_-]+/g, '-');
  return normalized || 'section';
}
