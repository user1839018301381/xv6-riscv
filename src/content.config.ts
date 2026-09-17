import { defineCollection } from 'astro:content';
import { glob } from 'astro/loaders';
import { z } from 'astro/zod';

const book = defineCollection({
  loader: glob({ pattern: '**/*.md', base: './content/book' }),
  schema: z.object({
    title: z.string(),
    chapter: z.number(),
    source: z.literal('xv6-riscv-book'),
    sourceFile: z.string(),
    commit: z.string()
  })
});

const bookJa = defineCollection({
  loader: glob({ pattern: '**/*.md', base: './content/book-ja' }),
  schema: z.object({
    title: z.string(),
    chapter: z.number(),
    source: z.literal('xv6-riscv-book'),
    sourceFile: z.string(),
    commit: z.string()
  })
});

export const collections = { book, bookJa };
