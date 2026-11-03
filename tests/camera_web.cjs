// Execute the actual JS embedded in the firmware, with deterministic browser/time stubs.
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const html = fs.readFileSync('src/web_console.html', 'utf8');
const js = html.split('<script>')[1].split('</script>')[0];
let now = 100, interval, timer, checks = 0, lastImage, capturedCanvas, download;
const context2d = {createImageData: (w,h) => ({data:new Uint8ClampedArray(w*h*4)}), putImageData(image){lastImage=image;}, drawImage(){}};
const elements = new Map();
const element = id => {
  if (!elements.has(id)) elements.set(id, {value:100, textContent:'', addEventListener(){}, getContext:()=>context2d,
    toBlob(cb){capturedCanvas=this;cb({});},click(){download=this.download;}});
  return elements.get(id);
};
const sockets = [];
class Socket {
  static OPEN = 1;
  constructor() { this.readyState = 1; this.sent = []; sockets.push(this); }
  send(s) {this.sent.push(s);}
  close() {this.readyState = 3; if(this.onclose)this.onclose();}
}
const sandbox = {document:{getElementById:element, createElement:type=>element(type==='canvas'?'raw':'anchor')},
  performance:{now:()=>now}, WebSocket:Socket, location:{host:'192.168.4.1'},
  setInterval:f=>{interval=f;}, setTimeout:f=>{timer=f;return 1;}, clearTimeout:()=>{timer=null;},
  ArrayBuffer, DataView, Uint8Array, Date, URL:{createObjectURL:()=> 'blob:test',revokeObjectURL(){}}};
vm.runInNewContext(js, sandbox);
const check = (ok, label) => {assert.ok(ok, label); checks++;};
const first = sockets[0]; first.onopen();
check(first.sent.at(-1)==='NEXT', 'first connection immediately asks for frame');
check(element('status').textContent.includes('chờ ảnh'), 'open socket is shown as waiting for image');
now += 2000; interval();
check(first.sent.filter(s=>s==='NEXT').length===2, 'missing first frame gets a new credit request');
const frame = new ArrayBuffer(640*480*2+16), header = new DataView(frame);
header.setUint32(0,0x4f564631); header.setUint16(4,640,true); header.setUint16(6,480,true); header.setUint32(8,1,true); header.setUint16(12,200,true);
header.setUint16(16,0xf800);header.setUint16(frame.byteLength-2,0x001f);
first.onmessage({data:frame});
check(element('cameraFps').textContent==='2.00' && element('status').textContent==='Đang nhận hình', 'VGA frame decodes and changes status');
check(element('raw').width===640 && element('raw').height===480, 'raw canvas uses native VGA pixels');
check(lastImage.data.length===640*480*4 && lastImage.data[0]===255 && lastImage.data[3]===255 && lastImage.data.at(-2)===255 && lastImage.data.at(-1)===255, 'entire VGA image decodes through last pixel');
check(first.sent.at(-1)==='NEXT', 'decoded image requests next frame');
const count = first.sent.length; element('pause').onclick(); now += 10000; interval();
check(sockets.length===1 && first.readyState===1 && first.sent.length===count+1, 'pause sends no watchdog requests and does not reconnect');
element('pause').onclick(); interval();
check(first.readyState===1, 'resume restarts watchdog deadline');
now += 9000; interval();
check(first.readyState===3 && typeof timer==='function', 'stalled open stream closes and schedules reconnect');
timer(); const second=sockets[1]; second.onopen();
check(second.sent.at(-1)==='NEXT', 'reconnection asks for first frame');
first.onmessage({data:frame});
check(second.sent.length===2, 'stale connection cannot send new credits after format negotiation');
second.onmessage({data:frame});
check(element('status').textContent==='Đang nhận hình', 'same sequence can recover after reconnect');
second.onmessage({data:frame.slice(0,-2)});
check(element('message').textContent.includes('không hợp lệ'), 'truncated VGA payload rejected');
const invalid = frame.slice(0); new DataView(invalid).setUint16(4,65535,true);
second.onmessage({data:invalid});
check(element('message').textContent.includes('không hợp lệ'), 'oversized dimensions rejected before allocation');
for (const [w,h] of [[320,240],[160,120]]) {
  const data=new ArrayBuffer(w*h*2+16), v=new DataView(data);
  v.setUint32(0,0x4f564631);v.setUint16(4,w,true);v.setUint16(6,h,true);v.setUint32(8,w,true);
  second.onmessage({data});
  check(element('raw').width===w && element('raw').height===h, `${w}x${h} protocol compatibility`);
  check(element('view').width===w && element('view').height===h, `${w}x${h} preview canvas preserves source dimensions`);
}
element('brightness').value=150; element('save').onclick();
check(second.sent.at(-1)==='CAPTURE' && !capturedCanvas,'save waits for fresh native frame instead of saving preview');
const snapshot=new ArrayBuffer(160*120*2+16), snapHeader=new DataView(snapshot);
snapHeader.setUint32(0,0x4f564631);snapHeader.setUint16(4,160,true);snapHeader.setUint16(6,120,true);snapHeader.setUint32(8,161,true);snapHeader.setUint8(14,2);
second.onmessage({data:snapshot});
check(capturedCanvas===element('raw'), 'PNG captures unfiltered native image instead of display canvas');
check(download.includes('160x120-frame161'), 'captured filename records source dimensions and snapshot sequence');
console.log(`PASS: ${checks} camera browser checks`);
