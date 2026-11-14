// Writes the third-party notices of the page: every package of the production dependency tree with its name, version and licence, and the text of its licence file
// when it has one. Run by ../build_site.sh; the result is put into the program's site (THIRD-PARTY-NOTICES.txt).
//   node make_notices.mjs <output file>
// The tree is the one `npm ls --omit=dev --all` shows: a little more than what the bundler kept (it drops what the page does not use), which is the safe side.
import { execSync } from 'node:child_process';
import { readdirSync, readFileSync, writeFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const listing = execSync('npm ls --omit=dev --all --parseable', { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 }).split(/\r?\n/).filter(Boolean).slice(1);
const seen = new Map();
for (const dir of listing) {
  const pkgFile = join(dir, 'package.json');
  if (!existsSync(pkgFile)) continue;
  const pkg = JSON.parse(readFileSync(pkgFile, 'utf8'));
  const key = pkg.name + '@' + pkg.version;
  if (seen.has(key)) continue;
  let spdx = pkg.license ?? (Array.isArray(pkg.licenses) ? pkg.licenses.map((l) => l.type).join(' OR ') : undefined);
  if (spdx && typeof spdx === 'object') spdx = spdx.type;
  const file = readdirSync(dir).find((f) => /^(licen[sc]e|copying|notice)(\.|$)/i.test(f) && !/\.(js|ts|json)$/i.test(f));
  seen.set(key, { name: pkg.name, version: pkg.version, spdx: spdx || (file ? 'no licence field; see its text below' : '(none declared)'), repository: typeof pkg.repository === 'string' ? pkg.repository : pkg.repository && pkg.repository.url, text: file ? readFileSync(join(dir, file), 'utf8').trim() : null });
}
const packages = [...seen.values()].sort((a, b) => a.name.localeCompare(b.name) || a.version.localeCompare(b.version));
const withText = packages.filter((p) => p.text);
let out = 'Third-party notices\n===================\n\n' +
  'This program contains the packages below (the page and the Excalidraw component are built from them). Each is under the licence named next to it.\n' +
  'The licence file of each package that has one is reproduced after the list.\n\n' +
  'Packages (' + packages.length + ')\n' + '-'.repeat(12) + '\n';
for (const p of packages) out += p.name + ' ' + p.version + '  ' + p.spdx + (p.text ? '' : '  (the package has no licence file; the licence is the one it declares)') + '\n';
out += '\n\nLicence texts (' + withText.length + ')\n' + '-'.repeat(16) + '\n';
for (const p of withText) out += '\n=== ' + p.name + ' ' + p.version + '  (' + p.spdx + ') ===\n' + p.text + '\n';
writeFileSync(process.argv[2], out);
const none = packages.filter((p) => p.spdx === '(none declared)').map((p) => p.name + '@' + p.version);
console.log(packages.length + ' packages, ' + withText.length + ' licence texts' + (none.length ? '; no licence declared: ' + none.join(', ') : ''));
