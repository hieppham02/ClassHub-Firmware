const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('src/web_console.html','utf8'),js=html.split('<script>')[2].split('</script>')[0];
let state={uid:'AABBCCDD',uidAgeMs:100,storageReady:true,readerReady:true,tags:[{uid:'AABBCCDD',name:'<img src=x onerror=alert(1)>'}],requestId:0,pending:false,result:'Ready',room:'DTD201',host:'broker',port:8883,mqttReady:true,wifiReady:true};
let timer,posted=[],checks=0,fail=false;
state.firmwareId='camera-jpeg-v7-miso19';state.firmwareBuild='test-build';state.readerReady=false;
state.rfidProbe={attempts:3,misoPin:19,fast:'FF,FF,FF',slow:'00,00,00',slowTested:true,error:'ESP_ERR_NOT_FOUND'};
function element(){return {value:'',textContent:'',disabled:false,children:[],replaceChildren(){this.children=[];},append(...items){this.children.push(...items);},click(){}};}
const elements=new Map(),get=id=>{if(!elements.has(id))elements.set(id,element());return elements.get(id);};
const context={document:{getElementById:get,createElement:element},setTimeout:f=>{timer=f;},URL:{createObjectURL:()=> 'blob:test',revokeObjectURL(){}},Blob,
  fetch:async (url,options={})=>{
    if(fail)throw Error('offline');
    if(options.method==='POST') {const payload=JSON.parse(options.body);posted.push({payload,headers:options.headers});state.requestId++;state.pending=true;return {ok:true,json:async()=>({requestId:state.requestId})};}
    return {ok:true,json:async()=>JSON.parse(JSON.stringify(state))};
  }};
const check=(ok,name)=>{assert.ok(ok,name);checks++;};
const flush=()=>new Promise(resolve=>setImmediate(resolve));
(async()=>{
  vm.runInNewContext(js,context);await flush();
  check(posted.length===0,'opening console only reads status and never actuates');
  check(get('firmwareId').textContent.includes('camera-jpeg-v7-miso19'),'web identifies diagnostic firmware actually running');
  check(get('rfidProbe').textContent.includes('GPIO19='),'web displays remapped MISO pin from device status');
  check(get('rfidProbe').textContent.includes('FF,FF,FF') && get('rfidProbe').textContent.includes('00,00,00') && get('rfidProbe').textContent.includes('ESP_ERR_NOT_FOUND'),'web shows both clock samples and reader failure without serial logs');
  check(get('tags').children[0].children[0].textContent.includes('<img'),'RFID label rendered as literal text');
  get('chooseUid').onclick();state.uid='11223344';await timer();await flush();
  get('tagName').value='TV remote';get('saveUid').onclick();await flush();
  check(posted[0].payload.uid==='AABBCCDD','save uses explicitly selected UID even when another tag arrives');
  check(posted[0].headers['X-ClassHub-Console']==='1','mutation includes same-origin console header');
  check(get('testOpen').disabled,'pending request disables actuator buttons');
  state.pending=false;state.result='OK: RFID saved';await timer();await flush();
  check(get('deviceResult').textContent===state.result&&!get('testOpen').disabled,'asynchronous completion shown before re-enabling controls');
  get('testMqtt').onclick();await flush();state.pending=false;state.result='Error: MQTT PUBACK timeout';await timer();await flush();
  check(get('deviceResult').textContent.includes('timeout'),'broker timeout shown as failure');
  get('testOpen').onclick();await flush();const count=posted.length;fail=true;await timer();await flush();
  check(posted.length===count&&get('testOpen').disabled,'lost connection never automatically retries physical action');
  fail=false;state.pending=false;state.result='OK: pulse started';await timer();await flush();
  check(!get('testOpen').disabled,'console recovers after status connection returns');
  console.log(`PASS: ${checks} device browser checks`);
})().catch(error=>{console.error(error);process.exitCode=1;});
