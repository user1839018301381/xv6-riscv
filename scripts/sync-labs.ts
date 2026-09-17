import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { load } from 'cheerio';
import {
  COURSE_SCHEDULE_URL,
  LABS,
  LAB_ASSET_DIR,
  LAB_BASE_URL,
  LAB_METADATA_PATH,
  LAB_PROCESSED_DIR,
  LAB_RAW_DIR,
  REQUEST_INTERVAL_MS,
  USER_AGENT
} from './lib/config.ts';
import { labAssetUrls, parseLab } from './lib/lab-parser.ts';
import type { LabMetadataFile } from './lib/types.ts';

const args = process.argv.slice(2);
const refresh = args.includes('--refresh');
const onlyFlag = args.find((arg) => arg.startsWith('--only='));
const onlyIndex = args.indexOf('--only');
const only = onlyFlag?.split('=')[1] ?? (onlyIndex >= 0 ? args[onlyIndex + 1] : undefined);
const selectedLabs = only ? LABS.filter((lab) => lab.slug === only) : [...LABS];

if (only && selectedLabs.length === 0) {
  throw new Error(`Unknown Lab slug: ${only}. Allowed: ${LABS.map((lab) => lab.slug).join(', ')}`);
}

let lastRequestAt = 0;
let connectionFailures = 0;

function blankMetadata(): LabMetadataFile {
  return {
    course: 'MIT 6.1810 Fall 2026',
    allowlist: LABS.map((lab) => lab.slug),
    listSource: COURSE_SCHEDULE_URL,
    listVerifiedAt: '',
    robots: {
      source: 'https://pdos.csail.mit.edu/robots.txt',
      checkedAt: '',
      status: 0
    },
    labs: {}
  };
}

async function loadMetadata(): Promise<LabMetadataFile> {
  if (!existsSync(LAB_METADATA_PATH)) return blankMetadata();
  return JSON.parse(await readFile(LAB_METADATA_PATH, 'utf8')) as LabMetadataFile;
}

async function waitForRequestSlot(): Promise<void> {
  const remaining = REQUEST_INTERVAL_MS - (Date.now() - lastRequestAt);
  if (remaining > 0) await new Promise((resolve) => setTimeout(resolve, remaining));
}

async function politeFetch(url: string, init: RequestInit = {}, allow404 = false): Promise<Response> {
  await waitForRequestSlot();
  let response: Response;
  try {
    response = await fetch(url, {
      ...init,
      headers: {
        'User-Agent': USER_AGENT,
        Accept: 'text/html,application/xhtml+xml,image/*;q=0.8,*/*;q=0.5',
        ...(init.headers ?? {})
      },
      redirect: 'follow',
      signal: AbortSignal.timeout(30_000)
    });
    connectionFailures = 0;
  } catch (error) {
    connectionFailures += 1;
    const message = error instanceof Error ? error.message : String(error);
    if (connectionFailures >= 2 || /reset|ECONNRESET/i.test(message)) {
      throw new Error(`Network acquisition stopped after connection failure: ${message}`);
    }
    throw error;
  } finally {
    lastRequestAt = Date.now();
  }

  if (response.status === 403) throw new Error(`MIT acquisition stopped: 403 from ${url}`);
  if (response.status === 429) {
    const retryAfter = response.headers.get('retry-after');
    throw new Error(`MIT acquisition stopped: 429 from ${url}${retryAfter ? `; Retry-After: ${retryAfter}` : ''}`);
  }
  if (response.status >= 500) throw new Error(`MIT acquisition stopped on server error ${response.status} from ${url}`);
  if (!response.ok && response.status !== 304 && !(allow404 && response.status === 404)) {
    throw new Error(`MIT acquisition stopped: ${response.status} from ${url}`);
  }
  return response;
}

function robotsAllows(robotsText: string, targetPath: string): boolean {
  let applies = false;
  for (const sourceLine of robotsText.split(/\r?\n/)) {
    const line = sourceLine.replace(/#.*$/, '').trim();
    if (!line) continue;
    const [field, ...rest] = line.split(':');
    const value = rest.join(':').trim();
    if (field.toLowerCase() === 'user-agent') applies = value === '*';
    if (applies && field.toLowerCase() === 'disallow' && value && targetPath.startsWith(value)) return false;
  }
  return true;
}

async function verifyRemotePolicy(metadata: LabMetadataFile): Promise<void> {
  const robotsUrl = metadata.robots.source;
  const robotsResponse = await politeFetch(robotsUrl, {}, true);
  const robotsText = robotsResponse.status === 404 ? '' : await robotsResponse.text();
  if (!robotsAllows(robotsText, '/6.S081/2026/labs/')) {
    throw new Error('robots.txt disallows /6.S081/2026/labs/. Acquisition stopped.');
  }
  metadata.robots = { source: robotsUrl, checkedAt: new Date().toISOString(), status: robotsResponse.status };

  const scheduleResponse = await politeFetch(COURSE_SCHEDULE_URL);
  const scheduleHtml = await scheduleResponse.text();
  const $ = load(scheduleHtml);
  const listed = new Set<string>();
  $('a[href^="labs/"]').each((_, element) => {
    const href = $(element).attr('href') ?? '';
    const match = href.match(/^labs\/([a-z0-9-]+)\.html$/);
    if (match && /^Lab\s/i.test($(element).text().trim())) listed.add(match[1]);
  });
  const expected = LABS.map((lab) => lab.slug);
  const actual = [...listed];
  const expectedSet = new Set<string>(expected);
  if (expected.some((slug) => !listed.has(slug)) || actual.some((slug) => !expectedSet.has(slug))) {
    throw new Error(`2026 Lab list changed. Expected [${expected.join(', ')}], official schedule has [${actual.join(', ')}]. Review before updating the explicit allowlist.`);
  }
  metadata.listVerifiedAt = new Date().toISOString();
}

function looksBlocked(html: string): boolean {
  return /cf-chl-|cloudflare ray id|attention required|captcha|verify you are human|access denied/i.test(html);
}

async function writeMetadata(metadata: LabMetadataFile): Promise<void> {
  await mkdir(path.dirname(LAB_METADATA_PATH), { recursive: true });
  await writeFile(LAB_METADATA_PATH, `${JSON.stringify(metadata, null, 2)}\n`);
}

async function sync(): Promise<void> {
  await Promise.all([
    mkdir(LAB_RAW_DIR, { recursive: true }),
    mkdir(LAB_PROCESSED_DIR, { recursive: true }),
    mkdir(LAB_ASSET_DIR, { recursive: true })
  ]);
  const metadata = await loadMetadata();
  const needsNetwork = selectedLabs.some((lab) => refresh || !existsSync(path.join(LAB_RAW_DIR, `${lab.slug}.html`)));
  if (needsNetwork) await verifyRemotePolicy(metadata);

  for (const lab of selectedLabs) {
    const source = new URL(`${lab.slug}.html`, LAB_BASE_URL).href;
    const rawPath = path.join(LAB_RAW_DIR, `${lab.slug}.html`);
    let fetchedAt = metadata.labs[lab.slug]?.fetchedAt ?? '';

    if (!existsSync(rawPath) || refresh) {
      const previous = metadata.labs[lab.slug];
      const headers: Record<string, string> = {};
      if (refresh && previous?.etag) headers['If-None-Match'] = previous.etag;
      if (refresh && previous?.lastModified) headers['If-Modified-Since'] = previous.lastModified;
      const response = await politeFetch(source, { headers });
      if (response.status === 304) {
        console.log(`${lab.slug}: unchanged (304)`);
      } else {
        const html = await response.text();
        if (looksBlocked(html)) throw new Error(`MIT acquisition stopped: block/challenge page returned for ${source}`);
        await writeFile(rawPath, html);
        fetchedAt = new Date().toISOString();
        metadata.labs[lab.slug] = {
          source,
          fetchedAt,
          etag: response.headers.get('etag'),
          lastModified: response.headers.get('last-modified'),
          assets: previous?.assets ?? []
        };
        console.log(`${lab.slug}: fetched`);
      }
    } else {
      console.log(`${lab.slug}: cached`);
    }

    const rawHtml = await readFile(rawPath, 'utf8');
    const assetMap = new Map<string, string>();
    const downloadedAssets: string[] = [];
    for (const assetUrl of labAssetUrls(rawHtml, source)) {
      const assetName = path.basename(new URL(assetUrl).pathname);
      const publicPath = `/labs-assets/${lab.slug}/${assetName}`;
      const outputPath = path.join(LAB_ASSET_DIR, lab.slug, assetName);
      assetMap.set(assetUrl, publicPath);
      downloadedAssets.push(assetUrl);
      if (!existsSync(outputPath) || refresh) {
        await mkdir(path.dirname(outputPath), { recursive: true });
        const response = await politeFetch(assetUrl);
        const bytes = new Uint8Array(await response.arrayBuffer());
        await writeFile(outputPath, bytes);
      }
    }

    const record = metadata.labs[lab.slug] ?? {
      source,
      fetchedAt: fetchedAt || new Date().toISOString(),
      etag: null,
      lastModified: null,
      assets: []
    };
    record.assets = downloadedAssets;
    metadata.labs[lab.slug] = record;
    const processed = parseLab(rawHtml, {
      slug: lab.slug,
      source,
      fetchedAt: record.fetchedAt,
      assetMap
    });
    await writeFile(path.join(LAB_PROCESSED_DIR, `${lab.slug}.json`), `${JSON.stringify(processed, null, 2)}\n`);
    console.log(`${lab.slug}: parsed (${processed.outline.length} sections, content guard passed)`);
  }

  await writeMetadata(metadata);
}

await sync();
