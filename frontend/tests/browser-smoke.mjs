import test from 'node:test';
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {readFile,stat,mkdir} from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {chromium} from 'playwright-core';

// Browser interaction tests use an explicit API fixture. They do not replace backend integration tests.
test('report, checks, pivot, configuration and stable-link browser flows', {timeout:120000}, async()=>{
 const capture=async(name)=>{if(process.env.UI_ARTIFACT_DIR){await mkdir(process.env.UI_ARTIFACT_DIR,{recursive:true});await page.screenshot({path:path.join(process.env.UI_ARTIFACT_DIR,name+'.png'),fullPage:true});}};
 const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..'),dist=path.join(root,'dist');
 await stat(path.join(dist,'index.html'));
 const server=createServer(async(req,res)=>{try{const url=new URL(req.url,'http://localhost'),relative=decodeURIComponent(url.pathname).replace(/^\/+/,''),file=path.resolve(dist,relative||'index.html');if(!file.startsWith(dist+path.sep)){res.writeHead(403).end();return;}const content=await readFile(file);const mime={'.html':'text/html','.js':'text/javascript','.css':'text/css','.svg':'image/svg+xml'}[path.extname(file)]||'application/octet-stream';res.writeHead(200,{'Content-Type':mime});res.end(content);}catch{res.writeHead(404).end();}});
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const url=`http://127.0.0.1:${server.address().port}`;
 const browser=await chromium.launch(process.env.BROWSER_EXECUTABLE?{executablePath:process.env.BROWSER_EXECUTABLE,headless:true}:{channel:'msedge',headless:true});
 const page=await browser.newPage({viewport:{width:1440,height:1000}}),errors=[],requests=[];page.on('pageerror',error=>errors.push(error.message));
 const settings={exclude_paths:[],array_rules:{},bucket_rules:{},aggregate_dimensions:['PATH','SOURCE','VALUE_ADJUSTED']};
 const stageProcedures=['normalize','compare','aggregate'].map(kind=>({id:kind,name:kind,kind,mode:'native'}));
 const checks=Array.from({length:5},(_,i)=>({id:`c${i+1}`,name:`Проверка ${i+1}`,kind:'check',params:{threshold:0.1,app_ids_limit:20},mode:'python',code:'def run(mart, params):\n    return []\nCHECK={"id":"test","label":"Test","params":{},"entrypoint":run}\n'}));
 const templates={normalize:'def normalize(records, config, emit):\n    return None\n',compare:'def compare(rows, config, emit):\n    return None\n',aggregate:'def aggregate(rows, union, segments, config, emit):\n    return None\n',check:checks[0].code};
 const old={id:'legacy',name:'Старая конфигурация',dataset_id:'ds1',normalize_id:'normalize',compare_id:'compare',aggregate_id:'aggregate',settings};let configs=[old],procedures=[...stageProcedures,...checks],authenticated=false;
 const rows=[{id:'1',check_id:'c1',CHECK_ID:'test1',LABEL:'Смена модели',SOURCE:'AFTER',ARGUMENT:'MODEL_A',CNT:9,TOTAL:20,METRIC:0.45,APP_IDS:['I1001'],RESULT:'FAIL',CONFIDENCE:'HIGH'},{id:'2',check_id:'c2',CHECK_ID:'test2',LABEL:'Слабый сигнал',SOURCE:'AFTER',ARGUMENT:'rare',CNT:1,TOTAL:20,METRIC:0.05,APP_IDS:['I1002'],RESULT:'FAIL',CONFIDENCE:'LOW'},{id:'3',check_id:'c3',CHECK_ID:'test3',LABEL:'Стабильный продукт',SOURCE:'BEFORE',ARGUMENT:'P1',CNT:0,TOTAL:20,METRIC:0,APP_IDS:[],RESULT:'PASS',CONFIDENCE:'HIGH'}];
 const run={id:'aa11',name:'Проверка стратегии',status:'completed',stage:'completed',created_at:'2026-11-06T18:00:00Z',snapshot:{checks,procedures:Object.fromEntries(stageProcedures.map(p=>[p.kind,p]))},summary:{total_apps:20,changed_apps:10,decision_changed:7,changed_without_decision:3,normalized_rows:100,comparison_rows:60,facts:25,total_seconds:2}};
 const contexts=[...['I1001','I1002'].flatMap(APP_ID=>['BEFORE','AFTER'].map(SOURCE=>({APP_ID,PATH:'decisionResult.score',SOURCE,CHANGE_TYPE:'VALUE_CHANGED',DIFF_FLAG:1,DECISION_CHANGED:1,PRODUCTS:['P1']}))),{APP_ID:'I1001',PATH:'decisionResult.reason',SOURCE:'AFTER',CHANGE_TYPE:'ATTRIBUTE_NEW',DIFF_FLAG:1,DECISION_CHANGED:1,PRODUCTS:['P1']}];
 const comparison={APP_ID:'I1001',PATH:'decisionResult.score',ARRAY_KEY:'',VALUE_BEFORE:'0.6',VALUE_AFTER:'0.7',PRESENT_BEFORE:true,PRESENT_AFTER:true,TYPE_BEFORE:'NUMBER',TYPE_AFTER:'NUMBER',DIFF_FLAG:1,CHANGE_TYPE:'VALUE_CHANGED'};
 await page.route('**/api/v1/**',async route=>{
  const req=route.request(),u=new URL(req.url()),p=u.pathname.replace('/api/v1',''),body=req.postDataJSON();requests.push({p,body,query:u.searchParams.toString()});let data={};
  if(p==='/me')data={user:authenticated?{username:'analyst'}:null};
  else if(p==='/login'){authenticated=true;data={user:{username:'analyst'}};}
  else if(p==='/datasets')data={datasets:[{id:'ds1',name:'Исходный набор',applications:20,files:40,path:'/data/RUN'}]};
  else if(p==='/defaults')data={settings};
  else if(p==='/procedure-templates')data=templates;
  else if(p==='/procedures'&&req.method()==='POST'){data={...body,id:'new-check',owner:'analyst'};procedures.push(data);}
  else if(p==='/procedures')data={procedures};
  else if(p==='/configs'&&req.method()==='POST'){data={...body,id:'new-config'};configs.push(data);}
  else if(p==='/configs')data={configs};
  else if(p==='/runs')data={runs:[run]};
  else if(p==='/runs/aa11')data={run};
  else if(p==='/runs/bb22')data={run:{...run,id:'bb22',status:'running',stage:'checking',progress:95}};
  else if(p.endsWith('/logs'))data={logs:[{created_at:run.created_at,stage:'checking',message:'Проверки выполняются',procedure_id:'c1'}]};
  else if(p==='/runs/aa11/report')data={run_id:run.id,summary:run.summary,checks,checks_summary:{total:3,pass:1,fail:2,low_confidence:1,checks:5}};
  else if(p==='/runs/aa11/report.html'){await route.fulfill({contentType:'text/html',headers:{'Content-Disposition':'attachment; filename="report-aa11.html"'},body:'<!doctype html><html><body>Mock exported report</body></html>'});return;}
  else if(p==='/runs/aa11/checks'){const filtered=rows.filter(r=>(!u.searchParams.get('result')||r.RESULT===u.searchParams.get('result'))&&(!u.searchParams.get('confidence')||r.CONFIDENCE===u.searchParams.get('confidence'))&&(!u.searchParams.get('check_id')||r.check_id===u.searchParams.get('check_id')));data={items:filtered,total:filtered.length};}
  else if(p==='/runs/aa11/query'||p==='/runs/aa11/query/apps'){
   const filtered=contexts.filter(r=>Object.entries(body.filters||{}).every(([k,v])=>JSON.stringify(r[k])===JSON.stringify(v)));
   if(p.endsWith('/apps')){const apps=[...new Set(filtered.map(r=>r.APP_ID))];data={total:apps.length,items:apps.map(APP_ID=>({APP_ID}))};}
   else{const groups=new Map();for(const r of filtered){const dimensions=Object.fromEntries(body.dimensions.map(d=>[d,r[d]??null])),key=JSON.stringify(dimensions);if(!groups.has(key))groups.set(key,{dimensions,apps:new Set()});groups.get(key).apps.add(r.APP_ID);}const items=[...groups.values()].map(g=>({...g.dimensions,CNT:g.apps.size}));data={total:items.length,items};}
  }
  else if(p==='/runs/aa11/apps/I1001')data={segment:{APP_ID:'I1001',DECISION_BEFORE:'APPROVE',DECISION_AFTER:'DECLINE'},comparison:[comparison],total:1};
  else if(p==='/runs/aa11/comparison')data={items:[comparison],total:1};
  else{await route.fulfill({status:404,json:{error:'Unknown fixture endpoint: '+p}});return;}
  await route.fulfill({json:data});
 });
 try{
  // The report URL survives login unchanged.
  await page.goto(`${url}/?run=aa11&tab=report`);await page.getByLabel('Имя пользователя').fill('analyst');await page.getByLabel('Пароль',{exact:true}).fill('test-password');await page.getByRole('button',{name:'Войти в пространство →'}).click();await page.getByRole('heading',{name:'Какие атрибуты изменились'}).waitFor();assert.equal(new URL(page.url()).searchParams.get('tab'),'report');
  assert.match(await page.locator('.metric').nth(3).innerText(),/3/);assert.equal(await page.locator('.checks-table .low-row').count(),1);
  const downloadPromise=page.waitForEvent('download');await page.getByRole('link',{name:'Скачать HTML ↓'}).click();const download=await downloadPromise;assert.match(download.suggestedFilename(),/\.html$/);assert.equal(await download.failure(),null);
  await page.getByRole('button',{name:'1 примеров ↗'}).first().click();await page.getByRole('region',{name:'Заявки проверки'}).waitFor();assert.match(await page.getByRole('region',{name:'Заявки проверки'}).innerText(),/не является полным/);await page.getByRole('link',{name:'I1001 ↗'}).click();await page.getByRole('cell',{name:'0.6 NUMBER',exact:true}).waitFor();
  await page.goto(`${url}/?run=aa11&tab=checks`);await page.getByLabel('Результат',{exact:true}).selectOption('PASS');await page.getByRole('button',{name:'Применить',exact:true}).click();await page.getByRole('cell',{name:'Стабильный продукт test3'}).waitFor();assert.equal(await page.locator('.checks-table tbody tr').count(),1);
  await page.goto(`${url}/?run=aa11&tab=pivot`);await page.getByRole('button',{name:'Заявки ячейки: 2',exact:true}).waitFor();assert.equal(await page.getByRole('button',{name:'Заявки ячейки: 4',exact:true}).count(),0);await page.getByRole('button',{name:'Заявки ячейки: 2',exact:true}).click();await page.getByRole('region',{name:'Заявки ячейки'}).waitFor();assert.equal(await page.getByRole('region',{name:'Заявки ячейки'}).locator('tbody tr').count(),2);
  await page.getByRole('button',{name:'Убрать CHANGE_TYPE',exact:true}).click();await page.getByLabel('Добавить измерение: Колонки').selectOption('SOURCE');await page.getByRole('button',{name:'Применить раскладку'}).click();await page.getByRole('columnheader',{name:'BEFORE',exact:true}).waitFor();assert.deepEqual(requests.filter(r=>r.p.endsWith('/query')).at(-1).body.dimensions,['PATH','SOURCE']);await capture('pivot');
  await page.getByLabel('Смена решения',{exact:true}).selectOption('1');await page.getByRole('button',{name:'Применить раскладку'}).click();await page.waitForTimeout(150);assert.equal(requests.filter(r=>r.p.endsWith('/query')).at(-1).body.filters.DECISION_CHANGED,1);
  await page.getByRole('button',{name:'Поменять оси ⇄'}).click();await page.waitForTimeout(150);assert.deepEqual(requests.filter(r=>r.p.endsWith('/query')).at(-1).body.dimensions,['SOURCE','PATH']);
  await page.goto(`${url}/#configs/legacy`);await page.getByRole('heading',{name:'Настроить конфигурацию'}).waitFor();assert.equal(await page.locator('.check-selection input[type=checkbox]:checked').count(),0);
  await page.goto(`${url}/#configs/new`);await page.getByRole('heading',{name:'Новая конфигурация',exact:true}).waitFor();assert.equal(await page.locator('.check-selection input[type=checkbox]:checked').count(),5);await page.locator('.check-option summary').first().click();await page.locator('.check-option textarea').first().fill('{"threshold":0.2,"app_ids_limit":1}');await page.getByRole('button',{name:'Сохранить',exact:true}).click();await page.waitForURL('**/#configs/new-config');assert.equal(configs.at(-1).checks.length,5);assert.equal(configs.at(-1).checks[0].params.threshold,0.2);
  await page.goto(`${url}/#procedures/new`);await page.getByLabel('Этап',{exact:true}).selectOption('check');await page.locator('.monaco-editor').waitFor();await page.getByRole('button',{name:'Сохранить свою процедуру'}).click();await page.waitForURL('**/#procedures/new-check');assert.equal(procedures.at(-1).kind,'check');assert.equal(procedures.at(-1).code,templates.check);
  await page.goto(`${url}/?run=bb22&tab=report`);await page.getByRole('heading',{name:'Журнал этапов'}).waitFor();assert.equal(await page.getByRole('link',{name:'Скачать HTML ↓'}).count(),0);assert(!requests.some(r=>r.p==='/runs/bb22/report'));
  await page.goto(`${url}/?run=aa11&tab=report`);await page.getByRole('heading',{name:'Какие атрибуты изменились'}).waitFor();await capture('report');await page.setViewportSize({width:390,height:844});await capture('report-mobile');assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);assert.deepEqual(errors,[]);
 }catch(error){await capture('failure');console.error('Browser errors:',errors,'Last requests:',requests.slice(-8),'Page:',(await page.locator('body').innerText()).slice(0,1500));throw error;}finally{await browser.close();await new Promise(resolve=>server.close(resolve));}
});
