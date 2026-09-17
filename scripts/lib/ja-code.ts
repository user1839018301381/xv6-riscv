/**
 * Restore source spelling for the small set of lab headings that are also
 * executable names.  These headings are outside <tt>/<code> in the cached
 * HTML, so a translation provider may otherwise localize them.
 */
const LAB_CODE_HEADINGS: Record<string, string> = {
  'lab-copy-on-write-fork-for-xv6': 'ラボ: xv6 のコピーオンライト fork',
  'implement-copy-on-write-fork': 'コピーオンライト fork の実装',
  sleep: 'sleep',
  sixfive: 'sixfive',
  memdump: 'memdump',
  find: 'find',
  exec: 'exec'
};

export function restoreLabHeading(id: string, text: string): string {
  return LAB_CODE_HEADINGS[id] ?? text;
}

export function restoreLabCodeHeadings(html: string): string {
  return html.replace(/(<h[1-3]\b[^>]*\bid="([^"]+)"[^>]*>)[\s\S]*?(<\/h[1-3]>)/gi, (match, opening: string, id: string, closing: string) => {
    const text = restoreLabHeading(id, '');
    return text ? `${opening}${text}${closing}` : match;
  });
}

export function restoreLabTitle(title: string): string {
  // Japanese characters are not treated as \b word characters by JavaScript,
  // so a simple literal replacement is safer for this title-only normalization.
  return title.replace(/フォーク/gu, 'fork');
}
