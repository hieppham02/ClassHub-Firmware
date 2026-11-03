const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const js=fs.readFileSync('src/web_console.html','utf8').split('<script>')[1].split('</script>')[0];
let sockets=[],elements=new Map(),decoded,closed=0,draws=0,downloads=0,checks=0,now=100,timer;
const ctx={createImageData:(w,h)=>({data:new Uint8ClampedArray(w*h*4)}),putImageData(){},drawImage(){draws++;}};
const get=id=>{if(!elements.has(id))elements.set(id,{value:100,textContent:'',disabled:false,addEventListener(){},getContext:()=>ctx,toBlob(cb){cb({});},click(){downloads++;}});return elements.get(id);};
class Ws{static OPEN=1;constructor(){this.readyState=1;this.sent=[];sockets.push(this);}send(text){this.sent.push(text);}close(){this.readyState=3;this.onclose?.();}}
const sandbox={document:{getElementById:get,createElement:type=>get(type==='canvas'?'raw':'a')},WebSocket:Ws,location:{host:'esp32'},performance:{now:()=>now},
  setInterval(){},setTimeout:f=>{timer=f;},clearTimeout(){},ArrayBuffer,DataView,Uint8Array,Blob,Date,URL:{createObjectURL:()=> 'blob:test',revokeObjectURL(){}},
  createImageBitmap:()=>new Promise(resolve=>{decoded=resolve;})};
const check=(ok,label)=>{assert.ok(ok,label);checks++;},flush=()=>new Promise(resolve=>setImmediate(resolve));
function packet(kind,seq=1,w=640,h=480){const data=new ArrayBuffer(kind===1?120:16+w*h*2),v=new DataView(data);v.setUint32(0,0x4f564631);v.setUint16(4,w,true);v.setUint16(6,h,true);v.setUint32(8,seq,true);v.setUint16(12,1250,true);v.setUint8(14,kind);return data;}
(async()=>{
  vm.runInNewContext(js,sandbox);const first=sockets[0];first.onopen();
  check(first.sent[0]==='FORMAT JPEG'&&first.sent.at(-1)==='NEXT','capable browser negotiates JPEG preview');
  first.onmessage({data:packet(1)});const sent=first.sent.length;
  check(typeof decoded==='function'&&first.sent.length===sent,'browser waits for JPEG decode before granting another frame');
  decoded({width:640,height:480,close(){closed++;}});await flush();
  check(first.sent.length===sent+1&&first.sent.at(-1)==='NEXT','decoded JPEG releases one next-frame credit');
  check(get('raw').width===640&&get('raw').height===480&&draws>0&&closed===1,'JPEG paints at source dimensions and closes bitmap');
  check(get('frameSize').textContent.includes('JPEG')&&get('cameraFps').textContent==='12.50','JPEG stats show actual source FPS and compressed size');
  get('save').onclick();check(first.sent.at(-1)==='CAPTURE'&&downloads===0,'JPEG preview is never downloaded as original photo');
  first.onmessage({data:packet(1,2)});decoded({width:640,height:480,close(){closed++;}});await flush();
  check(downloads===0,'in-flight JPEG cannot fulfill native snapshot request');
  get('pause').onclick();first.onmessage({data:packet(2,3)});
  check(downloads===1&&get('a').download.includes('640x480-frame3'),'native snapshot can be saved while preview is paused');
  get('pause').onclick();
  first.onmessage({data:packet(1,4)});decoded({width:320,height:240,close(){closed++;}});await flush();
  check(get('message').textContent.includes('JPEG')&&closed===3,'mismatched JPEG dimensions are rejected and bitmap released');
  first.onmessage({data:packet(1,5)});const oldDecode=decoded;first.close();timer();const second=sockets[1];second.onopen();const secondCount=second.sent.length;
  oldDecode({width:640,height:480,close(){closed++;}});await flush();
  check(second.sent.length===secondCount,'late JPEG from old socket cannot grant credits to replacement connection');
  console.log(`PASS: ${checks} JPEG preview and native snapshot browser checks`);
})().catch(error=>{console.error(error);process.exitCode=1;});
