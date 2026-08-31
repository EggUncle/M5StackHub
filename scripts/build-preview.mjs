// Documentation-only, source-derived illustration; never connects to hardware.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(root, 'src/main.cpp'), 'utf8');
function glyphs(start, end, expectedLength) {
  const body = source.slice(source.indexOf(start), source.indexOf(end));
  const result = {};
  for (const match of body.matchAll(/case '([^']+)': return ((?:"[01]+"\s*)+);/g)) {
    result[match[1]] = [...match[2].matchAll(/"([01]+)"/g)].map(x=>x[1]).join('');
    if (result[match[1]].length !== expectedLength) throw new Error('Invalid glyph '+match[1]);
  }
  if (!Object.keys(result).length) throw new Error('Glyph source anchors changed');
  return result;
}
export const font = glyphs('static const char* pixelGlyph3x5', 'static int pixelTextWidth', 15);
export const digits = glyphs('static const char* matrixDigit5x9', 'static void drawMatrixDigit', 45);
if (Object.keys(digits).length !== 10) throw new Error('Expected all ten digits');
const template = fs.readFileSync(path.join(root, 'docs/screenshots/template.html'), 'utf8');
fs.writeFileSync(path.join(root, 'docs/screenshots/preview.html'), template.replace('/* GLYPH_DATA */',
  'const FONT = '+JSON.stringify(font)+';\nconst DIGITS = '+JSON.stringify(digits)+';'));
console.log('Generated source-derived preview with '+Object.keys(font).length+' small glyphs and 10 digits.');
