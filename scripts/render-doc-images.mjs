// Offline, dependency-free documentation illustration. This is NOT a screenshot
// of the hardware or a browser. Do not use it to assert firmware visual QA.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {deflateSync} from 'node:zlib';
import {font, digits} from './build-preview.mjs';
const out = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../docs/screenshots');
const W=480,H=516;
let pixels;
function rect(x,y,w,h,c){
  x=Math.round(x);y=Math.round(y);
  for(let j=Math.max(0,y);j<Math.min(H,y+h);j++)
    for(let i=Math.max(0,x);i<Math.min(W,x+w);i++)
      for(let k=0;k<3;k++)pixels[(j*W+i)*3+k]=c[k];
}
function line(x0,y0,x1,y1,c){
  x0=Math.round(x0);y0=Math.round(y0);x1=Math.round(x1);y1=Math.round(y1);
  const dx=Math.abs(x1-x0),sx=x0<x1?1:-1,dy=-Math.abs(y1-y0),sy=y0<y1?1:-1;
  let err=dx+dy;
  while(true){rect(x0,y0,1,1,c);if(x0===x1&&y0===y1)break;const e=2*err;if(e>=dy){err+=dy;x0+=sx}if(e<=dx){err+=dx;y0+=sy}}
}
function outline(x,y,w,h,c){rect(x,y,w,1,c);rect(x,y+h-1,w,1,c);rect(x,y,1,h,c);rect(x+w-1,y,1,h,c)}
function circle(x,y,r,c){for(let j=-r;j<=r;j++)for(let i=-r;i<=r;i++)if(i*i+j*j<=r*r)rect(x+i,y+j,1,1,c)}
function width(str,s,t){return Math.max(0,[...str].reduce((w,ch)=>w+(ch===' '?3*s:3*s+t),0)-t)}
function text(str,x,y,s,t,c){for(const ch of str.toUpperCase()){if(ch===' '){x+=3*s;continue}const map=font[ch]||'000000000000000';for(let r=0;r<5;r++)for(let col=0;col<3;col++)if(map[r*3+col]==='1')rect(x+col*s,y+r*s,s,s,c);x+=3*s+t}}
function centered(str,x,y,s,t,c){text(str,x-Math.floor(width(str,s,t)/2),y,s,t,c)}
const warm=[216,215,205],ghost=[20,26,23],secondary=[94,108,114],cyan=[51,184,190];
function digit(ch,x,y){const map=digits[ch]||'00000'.repeat(4)+'11111'+'00000'.repeat(4);for(let r=0;r<9;r++)for(let col=0;col<5;col++)rect(x+col*9,y+r*8,7,7,map[r*5+col]==='1'?warm:ghost)}
function bezel(working){
  for(let tick=0;tick<60;tick++){const a=(-90+tick*6)*Math.PI/180,major=tick%5===0,inner=major?214:220;line(240+Math.cos(a)*inner,240+Math.sin(a)*inner,240+Math.cos(a)*228,240+Math.sin(a)*228,major?[66,76,86]:[34,43,51])}
  if(!working)return;
  const head=9.4,first=Math.floor(head)-4;
  for(let p=0;p<9;p++){
    const tick=first+p,d=Math.abs(tick-head);if(d>=4.5)continue;
    const shaped=Math.sin((1-d/4.5)*Math.PI/2)**2,major=tick%5===0;
    const len=(major?12:8)+Math.round(7*shaped),thickness=1+Math.round(2*shaped),a=(-90+tick*6)*Math.PI/180,rx=Math.cos(a),ry=Math.sin(a),strength=.18+shaped*.82;
    const c=[23+(76-23)*strength,56+(184-56)*strength,60+(195-60)*strength].map(Math.floor);
    for(let w=-Math.floor(thickness/2);w<=Math.floor(thickness/2);w++)line(240+rx*(228-len)-ry*w,240+ry*(228-len)+rx*w,240+rx*228-ry*w,240+ry*228+rx*w,c);
  }
}
function gauge(cx,percent,c){for(let i=0;i<10;i++){const x=cx-48+i*10;rect(x,340,7,6,[31,37,44]);if(percent>=0){const w=Math.round(7*Math.max(0,Math.min(1,(percent-i*10)/10)));if(w)rect(x,340,w,6,c)}}}
function render(state){
  pixels=Buffer.alloc(W*H*3);
  const unknown=state==='unsynced';
  const [label,color]={working:['WORKING',[55,117,124]],ready:['READY',[61,128,91]],needs_input:['NEEDS INPUT',[164,111,48]],unsynced:['IDLE',[76,96,106]]}[state];
  bezel(state==='working');
  centered(unknown?'TIME SYNC':'AUG 30  SUN',unknown?240:160,74,2,2,warm);
  if(!unknown){
    const c=[85,142,164];circle(281,79,7,c);circle(290,75,9,c);rect(273,79,30,10,c);
    text('29',311,74,2,2,c);outline(328,72,4,4,c);text('C',336,74,2,2,c);
    // Firmware uses efontCN_16 for 余杭. Avoid bundling third-party font data;
    // the illustration deliberately substitutes an explicitly documented label.
    centered('YUHANG',337,96,2,1,secondary);
  }
  let x=130;for(const ch of unknown?'----':'1021'){if(x===234){rect(x+3,172,7,7,cyan);rect(x+3,196,7,7,cyan);x+=20}digit(ch,x,148);x+=52}
  rect(120,238,240,1,[35,46,52]);rect(144,327,192,1,[35,46,52]);
  outline(146,267,10,10,color);rect(149,270,4,4,color);centered(label,240,267,3,3,color);
  centered(unknown?'MODEL --':'GPT-5.6-SOL',240,297,2,2,unknown?[67,75,79]:secondary);
  for(const [cx,name,value,suffix,c] of [[145,'QUOTA',86,'LEFT',[74,111,188]],[240,'CPU',12,'USED',[184,68,74]],[335,'MEM',59,'USED',[190,148,38]]]){
    gauge(cx,unknown?-1:value,c);centered(name,cx,358,2,2,c);centered(unknown?'--':value+'%',cx,378,4,3,c);centered(suffix,cx,407,2,2,secondary);
  }
  rect(0,480,480,36,[32,35,40]);
  centered('SOFTWARE PREVIEW - SAMPLE DATA',240,487,1,2,[200,205,210]);
  centered('NOT A DEVICE PHOTO',240,501,1,2,[200,205,210]);
  return pixels;
}
function crc32(buf){let crc=0xffffffff;for(const b of buf){crc^=b;for(let n=0;n<8;n++)crc=(crc>>>1)^((crc&1)?0xedb88320:0)}return(crc^0xffffffff)>>>0}
function chunk(type,data){const name=Buffer.from(type),body=Buffer.concat([name,data]),header=Buffer.alloc(4),tail=Buffer.alloc(4);header.writeUInt32BE(data.length);tail.writeUInt32BE(crc32(body));return Buffer.concat([header,body,tail])}
function png(rgb){
  const ihdr=Buffer.alloc(13);ihdr.writeUInt32BE(W,0);ihdr.writeUInt32BE(H,4);ihdr[8]=8;ihdr[9]=2;
  const raw=Buffer.alloc(H*(W*3+1));for(let y=0;y<H;y++)rgb.copy(raw,y*(W*3+1)+1,y*W*3,(y+1)*W*3);
  return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',ihdr),chunk('IDAT',deflateSync(raw)),chunk('IEND',Buffer.alloc(0))]);
}
for(const state of ['working','ready','needs_input','unsynced']){
  const file=path.join(out,state+'.png');fs.writeFileSync(file,png(render(state)));console.log('Rendered '+path.basename(file));
}
