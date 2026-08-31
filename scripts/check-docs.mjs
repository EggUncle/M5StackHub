// Validate local documentation links and generated PNG structure without
// accessing a browser, hardware, or the network.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {inflateSync} from 'node:zlib';
import {Script} from 'node:vm';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
function walk(dir){return fs.readdirSync(dir,{withFileTypes:true}).flatMap(e=>e.isDirectory()?walk(path.join(dir,e.name)):[path.join(dir,e.name)])}
const docs=[path.join(root,'README.md'),...walk(path.join(root,'docs')).filter(f=>f.endsWith('.md'))];
let links=0;
for(const file of docs){
  const body=fs.readFileSync(file,'utf8');
  for(const match of body.matchAll(/\[[^\]]*\]\(([^)]+)\)/g)){
    const target=match[1];if(/^(https?:|mailto:|#)/.test(target))continue;
    const [relative]=target.split('#');
    if(path.isAbsolute(relative))throw new Error('Nonportable link in '+path.relative(root,file));
    if(!fs.existsSync(path.resolve(path.dirname(file),decodeURIComponent(relative))))throw new Error('Missing link: '+target);
    links++;
  }
}
const imageNames=['working','ready','needs_input','unsynced'];
const signatures=new Set();
function crc32(buf){let crc=0xffffffff;for(const b of buf){crc^=b;for(let n=0;n<8;n++)crc=(crc>>>1)^((crc&1)?0xedb88320:0)}return(crc^0xffffffff)>>>0}
for(const name of imageNames){
  const data=fs.readFileSync(path.join(root,'docs/screenshots',name+'.png'));
  if(data.subarray(0,8).toString('hex')!=='89504e470d0a1a0a')throw new Error('Invalid PNG '+name);
  let offset=8,width,height;const chunks=[];let ended=false;
  while(offset<data.length){
    const size=data.readUInt32BE(offset),type=data.toString('ascii',offset+4,offset+8),body=data.subarray(offset+8,offset+8+size);
    const checksum=data.readUInt32BE(offset+8+size);
    if(checksum!==crc32(data.subarray(offset+4,offset+8+size)))throw new Error('PNG checksum '+name);
    if(type==='IHDR'){width=body.readUInt32BE(0);height=body.readUInt32BE(4);if(body[8]!==8||body[9]!==2)throw new Error('Expected RGB8 '+name)}
    if(type==='IDAT')chunks.push(body);
    offset+=12+size;
    if(type==='IEND'){ended=true;break}
  }
  if(!ended||offset!==data.length||width!==480||height!==516)throw new Error('PNG dimensions/chunks '+name);
  const raw=inflateSync(Buffer.concat(chunks));
  if(raw.length!==height*(width*3+1))throw new Error('PNG payload '+name);
  signatures.add(data.toString('base64'));
}
if(signatures.size!==4)throw new Error('Expected four distinct state illustrations');
const html=fs.readFileSync(path.join(root,'docs/screenshots/preview.html'),'utf8');
const code=html.match(/<script>([\s\S]*?)<\/script>/)?.[1];
if(!code||html.includes('/* GLYPH_DATA */'))throw new Error('Preview was not generated');
new Script(code); // Syntax only: deliberately does not execute the page.
console.log(`Checked ${docs.length} Markdown files, ${links} local links, four 480x516 PNGs and preview JS syntax.`);
