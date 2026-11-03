const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const js=fs.readFileSync('src/web_console.html','utf8').split('<script>')[3].split('</script>')[0];
let status={otaMaxBytes:3145728,pending:false,otaActive:false},requests=[],timers=[],reloads=0,checks=0,pauseClicks=0;
const elements=new Map(),get=id=>{if(!elements.has(id))elements.set(id,{disabled:false,value:0,textContent:'',files:[]});return elements.get(id);};
class Xhr {
  constructor(){this.upload={};this.headers={};requests.push(this);}
  open(method,url){this.method=method;this.url=url;}
  setRequestHeader(key,value){this.headers[key]=value;}
  send(file){this.file=file;}
}
const sandbox={document:{getElementById:get},paused:false,connected:()=>true,
  fetch:async()=>({ok:true,json:async()=>status}),XMLHttpRequest:Xhr,
  setTimeout:(fn,delay)=>{timers.push({fn,delay});},location:{reload:()=>reloads++}};
get('pause').onclick=()=>{sandbox.paused=!sandbox.paused;pauseClicks++;};
const check=(ok,label)=>{assert.ok(ok,label);checks++;};
const flush=()=>new Promise(resolve=>setImmediate(resolve));
const file=(name='firmware.bin',size=1024)=>{get('otaFile').files=[{name,size}];get('otaFile').onchange();};
(async()=>{
  vm.runInNewContext(js,sandbox);check(requests.length===0,'opening page never starts firmware update');
  file('bad.txt');await get('otaUpload').onclick();check(requests.length===0,'non-bin file rejected locally');
  file('firmware.bin',4000000);await get('otaUpload').onclick();check(requests.length===0 && !sandbox.classhubOtaUploading,'oversized image rejected before upload');
  check(pauseClicks===2 && !sandbox.paused,'preflight failure resumes previously running camera');
  file();status.otaMaxBytes=0;await get('otaUpload').onclick();check(get('otaResult').textContent.includes('USB'),'missing OTA layout explains first USB installation');
  status.otaMaxBytes=3145728;status.pending=true;await get('otaUpload').onclick();check(requests.length===0,'pending cabinet request prevents upload');
  status.pending=false;await get('otaUpload').onclick();await flush();
  let xhr=requests.at(-1);check(xhr.url==='/api/ota' && xhr.headers['X-ClassHub-Console']==='1' && xhr.file===get('otaFile').files[0],'upload sends raw selected file with console header');
  check(get('otaUpload').disabled && get('otaFile').disabled && sandbox.paused,'upload freezes file controls and pauses stream');
  xhr.upload.onprogress({lengthComputable:true,loaded:1024,total:1024});check(get('otaResult').textContent.includes('kiểm tra'),'100 percent upload still waits for image verification');
  xhr.status=400;xhr.responseText='Image verification failed';xhr.onload();check(!sandbox.classhubOtaUploading && !get('otaUpload').disabled,'invalid image response restores retry controls');
  await get('otaUpload').onclick();xhr=requests.at(-1);const count=requests.length;xhr.onerror();check(requests.length===count && timers.length===0,'network error never automatically resends firmware');
  await get('otaUpload').onclick();xhr=requests.at(-1);xhr.status=200;xhr.responseText='{"ok":true,"rebooting":true}';xhr.onload();
  check(timers.length===1 && timers[0].delay===10000 && get('otaUpload').disabled,'verified update waits for restart before page reload');
  timers[0].fn();check(reloads===1,'page reloads once after confirmed update');
  console.log(`PASS: ${checks} OTA browser checks`);
})().catch(error=>{console.error(error);process.exitCode=1;});
