import { defineConfig } from 'astro/config';
import { unified } from '@astrojs/markdown-remark';
import rehypeHighlight from 'rehype-highlight';
import rehypeKatex from 'rehype-katex';
import remarkMath from 'remark-math';

export default defineConfig({
  output: 'static',
  markdown: {
    syntaxHighlight: false,
    processor: unified({
      remarkPlugins: [remarkMath],
      rehypePlugins: [rehypeHighlight, rehypeKatex]
    })
  },
  vite: {
    build: {
      cssMinify: 'lightningcss'
    }
  }
});
