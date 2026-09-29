import { load } from 'cheerio';
import { chapterPath, sectionPath } from './config.ts';

export interface BookSection {
  chapter: number;
  number: number;
  title: string;
  headingId?: string;
  html: string;
  ids: string[];
}

export interface BookChapter {
  chapter: number;
  title: string;
  sections: BookSection[];
}

export interface BookTocChapter {
  chapter: number;
  title: string;
  sections: { number: number; title: string }[];
}

export function bookToc(chapters: BookChapter[]): BookTocChapter[] {
  return chapters.map(({ chapter, title, sections }) => ({
    chapter,
    title,
    sections: sections.slice(1).map(({ number, title }) => ({ number, title }))
  }));
}

interface RenderedChapter {
  data: { chapter: number; title: string };
  rendered?: { html: string };
}

export function splitBookChapter(entry: RenderedChapter): BookChapter {
  const { chapter, title } = entry.data;
  const $ = load(entry.rendered?.html ?? '', null, false);
  const sections: BookSection[] = [{ chapter, number: 0, title, html: '', ids: [] }];

  for (const node of $.root().contents().toArray()) {
    if (node.type === 'tag' && node.tagName === 'h2') {
      const headingId = $(node).attr('id');
      sections.push({ chapter, number: sections.length, title: $(node).text(), headingId, html: '', ids: headingId ? [headingId] : [] });
      continue;
    }
    const section = sections.at(-1)!;
    section.html += $.html(node);
    section.ids.push(...$(node).find('[id]').addBack('[id]').map((_, element) => $(element).attr('id')!).get());
  }

  return { chapter, title, sections };
}

export function linkBookSections(chapters: BookChapter[]): BookChapter[] {
  const destinations = new Map<string, string>();
  const globalDestinations = new Map<string, string>();
  const duplicateIds = new Set<string>();
  for (const chapter of chapters) {
    for (const section of chapter.sections) {
      const path = section.number === 0 ? chapterPath(chapter.chapter) : sectionPath(chapter.chapter, section.number);
      for (const id of section.ids) {
        const destination = `${path}#${id}`;
        destinations.set(`${chapter.chapter}#${id}`, destination);
        if (globalDestinations.has(id)) duplicateIds.add(id);
        else globalDestinations.set(id, destination);
      }
    }
  }
  for (const id of duplicateIds) globalDestinations.delete(id);

  return chapters.map((chapter) => ({
    ...chapter,
    sections: chapter.sections.map((section) => {
      const $ = load(section.html, null, false);
      $('a[href]').each((_, anchor) => {
        const href = $(anchor).attr('href')!;
        const match = href.match(/^(?:\/book\/chapter-(\d+)\/)?#(.+)$/);
        if (!match) return;
        const targetChapter = match[1] ? Number(match[1]) : chapter.chapter;
        const id = decodeURIComponent(match[2]);
        const destination = destinations.get(`${targetChapter}#${id}`) ?? (!match[1] ? globalDestinations.get(id) : undefined);
        if (destination) $(anchor).attr('href', destination);
        else if (id.startsWith('CH:')) $(anchor).attr('href', chapterPath(targetChapter));
      });
      return { ...section, html: $.html() };
    })
  }));
}
