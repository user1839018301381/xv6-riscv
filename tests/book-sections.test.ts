import assert from 'node:assert/strict';
import test from 'node:test';
import { linkBookSections, splitBookChapter } from '../scripts/lib/book-sections.ts';

test('Book sections get their own pages and cross-chapter figure links follow the figure', () => {
  const first = splitBookChapter({
    data: { chapter: 1, title: 'First' },
    rendered: { html: '<p>Introduction</p><h2 id="one">One</h2><p><a href="#fig:other">Figure</a></p><h2 id="two">Two</h2><p><a href="#one">Back</a></p>' }
  });
  const second = splitBookChapter({
    data: { chapter: 2, title: 'Second' },
    rendered: { html: '<p>Introduction</p><h2 id="figure">Figure</h2><figure id="fig:other"></figure>' }
  });
  const [linkedFirst] = linkBookSections([first, second]);

  assert.equal(linkedFirst.sections.length, 3);
  assert.equal(linkedFirst.sections[0].html, '<p>Introduction</p>');
  assert.equal(linkedFirst.sections[1].headingId, 'one');
  assert.doesNotMatch(linkedFirst.sections[1].html, /<h2/);
  assert.match(linkedFirst.sections[1].html, /href="\/book\/chapter-2\/section-1\/#fig:other"/);
  assert.match(linkedFirst.sections[2].html, /href="\/book\/chapter-1\/section-1\/#one"/);
});
