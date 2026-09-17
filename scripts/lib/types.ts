export interface ContentStats {
  h1: number;
  h2: number;
  h3: number;
  paragraphs: number;
  codeBlocks: number;
  links: number;
  lists: number;
  tables: number;
  figures: number;
}

export interface OutlineItem {
  depth: 2 | 3;
  id: string;
  text: string;
  number: string;
}

export interface LabSourceMetadata {
  source: string;
  fetchedAt: string;
  etag: string | null;
  lastModified: string | null;
  assets: string[];
}

export interface LabMetadataFile {
  course: string;
  allowlist: string[];
  listSource: string;
  listVerifiedAt: string;
  robots: {
    source: string;
    checkedAt: string;
    status: number;
  };
  labs: Record<string, LabSourceMetadata>;
}

export interface ProcessedLab {
  slug: string;
  title: string;
  source: string;
  fetchedAt: string;
  outline: OutlineItem[];
  html: string;
  sourceStats: ContentStats;
  renderedStats: ContentStats;
}

export interface SearchRecord {
  id: string;
  kind: 'Lab' | 'Book';
  collection: string;
  title: string;
  section: string;
  url: string;
  text: string;
  tokens: string;
}

export interface BookMarkdownStats {
  headings: number;
  codeBlocks: number;
  figures: number;
  tables: number;
  footnotes: number;
}

export interface BookConversionRecord {
  chapter: number;
  file: string;
  source: BookMarkdownStats;
  output: BookMarkdownStats;
}
