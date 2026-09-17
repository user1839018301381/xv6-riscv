const root = document.documentElement;
const themeButton = document.querySelector('[data-theme-toggle]');
const preferredDark = () => window.matchMedia('(prefers-color-scheme: dark)').matches;

function currentTheme() {
  return root.dataset.theme || (preferredDark() ? 'dark' : 'light');
}

function updateThemeLabel() {
  if (!themeButton) return;
  const next = currentTheme() === 'dark' ? 'light' : 'dark';
  themeButton.setAttribute('aria-label', `Switch to ${next} theme`);
}

themeButton?.addEventListener('click', () => {
  const next = currentTheme() === 'dark' ? 'light' : 'dark';
  root.dataset.theme = next;
  localStorage.setItem('xv6-reader-theme', next);
  updateThemeLabel();
});
updateThemeLabel();

const dialog = document.querySelector('[data-search-dialog]');
const searchInput = document.querySelector('[data-search-input]');
const resultsNode = document.querySelector('[data-search-results]');
const statusNode = document.querySelector('[data-search-status]');
let searchRecords;
let visibleResults = [];
let activeIndex = -1;

async function loadSearch() {
  if (searchRecords) return searchRecords;
  const response = await fetch('/search-index.json');
  if (!response.ok) throw new Error(`Search index returned ${response.status}`);
  searchRecords = (await response.json()).records;
  return searchRecords;
}

function scoreRecord(record, terms) {
  let score = 0;
  const title = `${record.title} ${record.section}`.toLocaleLowerCase('en-US');
  for (const term of terms) {
    if (!record.tokens.includes(term)) return 0;
    if (title === term) score += 20;
    else if (title.startsWith(term)) score += 12;
    else if (title.includes(term)) score += 8;
    else score += 2;
  }
  return score;
}

function renderSearch() {
  if (!resultsNode || !statusNode) return;
  resultsNode.replaceChildren();
  activeIndex = visibleResults.length ? 0 : -1;
  visibleResults.forEach((record, index) => {
    const link = document.createElement('a');
    link.href = record.url;
    link.className = 'search-result';
    link.setAttribute('role', 'option');
    link.setAttribute('aria-selected', String(index === activeIndex));
    const source = document.createElement('span');
    source.className = 'search-result__source';
    source.textContent = `${record.kind} / ${record.collection}`;
    const title = document.createElement('strong');
    title.textContent = record.section || record.title;
    const excerpt = document.createElement('span');
    excerpt.className = 'search-result__excerpt';
    excerpt.textContent = record.text.slice(0, 180);
    link.append(source, title, excerpt);
    resultsNode.append(link);
  });
  statusNode.textContent = visibleResults.length ? `${visibleResults.length} results` : 'No matching sections.';
}

async function search(query) {
  const terms = query.trim().toLocaleLowerCase('en-US').split(/\s+/).filter(Boolean);
  if (!terms.length) {
    visibleResults = [];
    if (resultsNode) resultsNode.replaceChildren();
    if (statusNode) statusNode.textContent = 'Type to search all Labs and Book chapters.';
    return;
  }
  try {
    const records = await loadSearch();
    visibleResults = records
      .map((record) => ({ record, score: scoreRecord(record, terms) }))
      .filter((item) => item.score > 0)
      .sort((a, b) => b.score - a.score)
      .slice(0, 18)
      .map((item) => item.record);
    renderSearch();
  } catch {
    if (statusNode) statusNode.textContent = 'Search index is unavailable. Run npm run build again.';
  }
}

function openSearch() {
  if (!(dialog instanceof HTMLDialogElement)) return;
  dialog.showModal();
  document.querySelector('#main-content')?.setAttribute('inert', '');
  requestAnimationFrame(() => searchInput?.focus());
}

function closeSearch() {
  if (!(dialog instanceof HTMLDialogElement)) return;
  dialog.close();
  document.querySelector('#main-content')?.removeAttribute('inert');
}

document.querySelectorAll('[data-search-open]').forEach((button) => button.addEventListener('click', openSearch));
document.querySelector('[data-search-close]')?.addEventListener('click', closeSearch);
dialog?.addEventListener('click', (event) => {
  if (event.target === dialog) closeSearch();
});
dialog?.addEventListener('close', () => document.querySelector('#main-content')?.removeAttribute('inert'));
document.addEventListener('keydown', (event) => {
  if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'k') {
    event.preventDefault();
    openSearch();
  }
});
searchInput?.addEventListener('input', (event) => search(event.target.value));
searchInput?.addEventListener('keydown', (event) => {
  if (!visibleResults.length) return;
  if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
    event.preventDefault();
    activeIndex = (activeIndex + (event.key === 'ArrowDown' ? 1 : -1) + visibleResults.length) % visibleResults.length;
    resultsNode?.querySelectorAll('.search-result').forEach((item, index) => {
      item.setAttribute('aria-selected', String(index === activeIndex));
      if (index === activeIndex) item.scrollIntoView({ block: 'nearest' });
    });
  }
  if (event.key === 'Enter' && activeIndex >= 0) {
    event.preventDefault();
    window.location.href = visibleResults[activeIndex].url;
  }
});

document.querySelectorAll('.prose pre').forEach((pre) => {
  const wrapper = document.createElement('div');
  wrapper.className = 'code-shell';
  pre.parentNode?.insertBefore(wrapper, pre);
  wrapper.append(pre);
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'copy-button';
  button.textContent = 'Copy';
  button.setAttribute('aria-label', 'Copy code');
  button.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(pre.textContent || '');
      button.textContent = 'Copied';
      button.dataset.state = 'success';
      window.setTimeout(() => {
        button.textContent = 'Copy';
        delete button.dataset.state;
      }, 2500);
    } catch {
      button.textContent = 'Copy failed';
      button.dataset.state = 'error';
    }
  });
  wrapper.append(button);
});

const tocLinks = new Map(
  [...document.querySelectorAll('.article-toc a, .mobile-toc a')]
    .map((link) => [decodeURIComponent(link.hash.slice(1)), link])
);
const observedHeadings = [...document.querySelectorAll('.prose h2[id], .prose h3[id]')];
if (observedHeadings.length && 'IntersectionObserver' in window) {
  const observer = new IntersectionObserver((entries) => {
    const visible = entries.filter((entry) => entry.isIntersecting).sort((a, b) => a.boundingClientRect.top - b.boundingClientRect.top)[0];
    if (!visible) return;
    document.querySelectorAll('.article-toc a[aria-current], .mobile-toc a[aria-current]').forEach((link) => link.removeAttribute('aria-current'));
    document.querySelectorAll(`a[href="#${CSS.escape(visible.target.id)}"]`).forEach((link) => link.setAttribute('aria-current', 'location'));
    history.replaceState(null, '', `#${visible.target.id}`);
  }, { rootMargin: '-15% 0px -72% 0px' });
  observedHeadings.forEach((heading) => observer.observe(heading));
}
