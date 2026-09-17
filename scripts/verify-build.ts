import { existsSync } from 'node:fs';
import { readFile, readdir } from 'node:fs/promises';
import path from 'node:path';
import { load } from 'cheerio';
import { PROJECT_ROOT } from './lib/config.ts';

const outputDirectory = path.join(PROJECT_ROOT, 'dist');
const failures = new Set<string>();

async function htmlFiles(directory: string): Promise<string[]> {
  const found: string[] = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const target = path.join(directory, entry.name);
    if (entry.isDirectory()) found.push(...await htmlFiles(target));
    else if (entry.name.endsWith('.html')) found.push(target);
  }
  return found;
}

function outputTarget(urlPath: string): string {
  const clean = decodeURIComponent(urlPath.split(/[?#]/)[0]);
  if (clean.endsWith('/')) return path.join(outputDirectory, clean, 'index.html');
  const direct = path.join(outputDirectory, clean);
  if (path.extname(clean)) return direct;
  return path.join(direct, 'index.html');
}

for (const file of await htmlFiles(outputDirectory)) {
  const $ = load(await readFile(file, 'utf8'));
  $('[href], [src]').each((_, element) => {
    const value = $(element).attr('href') ?? $(element).attr('src');
    if (!value || !value.startsWith('/') || value.startsWith('//')) return;
    if (!existsSync(outputTarget(value))) {
      failures.add(`${path.relative(outputDirectory, file)} -> ${value}`);
    }
  });
}

if (failures.size > 0) {
  console.error([...failures].map((failure) => `- ${failure}`).join('\n'));
  process.exitCode = 1;
} else {
  console.log('build: all internal links and assets resolve');
}
