import assert from 'node:assert/strict';
import test from 'node:test';
import { extractLabBody, parseLab } from '../scripts/lib/lab-parser.ts';

const fixture = `
<html><head><title>Ignored shell</title></head><body>
<!-- START SECTION: body -->
<h1>Lab: Fixture</h1>
<p>Keep this paragraph and <a href="notes.html">this link</a>.</p>
<p>Read Chapter 3 of the <a href="https://mit-pdos.github.io/xv6-riscv-book/">xv6 book</a>.</p>
<h2>First task</h2>
<pre>$ make qemu</pre>
<h3>Check it</h3>
<ul><li>One</li><li>Two</li></ul>
<!-- END SECTION: body -->
</body></html>`;

test('extractLabBody keeps only the official body boundary', () => {
  const body = extractLabBody(fixture);
  assert.match(body, /Lab: Fixture/);
  assert.doesNotMatch(body, /Ignored shell/);
});

test('parseLab preserves protected content and creates stable navigation', () => {
  const parsed = parseLab(fixture, {
    slug: 'fixture',
    source: 'https://example.test/labs/fixture.html',
    fetchedAt: '2026-09-17T00:00:00.000Z'
  });

  assert.equal(parsed.title, 'Lab: Fixture');
  assert.deepEqual(parsed.renderedStats, parsed.sourceStats);
  assert.deepEqual(parsed.outline, [
    { depth: 2, id: 'first-task', text: 'First task', number: '01' },
    { depth: 3, id: 'check-it', text: 'Check it', number: '01.1' }
  ]);
  assert.match(parsed.html, /href="https:\/\/example\.test\/labs\/notes\.html"/);
  assert.match(parsed.html, /href="\/book\/chapter-3\/" class="book-link"/);
  assert.match(parsed.html, /class="hljs language-bash"/);
});

test('parseLab stops when MIT body markers change', () => {
  assert.throws(
    () => parseLab('<h1>Unbounded</h1>', {
      slug: 'fixture',
      source: 'https://example.test/labs/fixture.html',
      fetchedAt: '2026-09-17T00:00:00.000Z'
    }),
    /expected body boundary comments/
  );
});
