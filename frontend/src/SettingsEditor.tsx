import {useState} from 'react';
import type {Row} from './api';

const dimensions=['PREFIX','CLASS','SUFFIX','ARRAY_KEY','KEY','PATH','VALUE_ADJUSTED','SOURCE','DIFF_FLAG','CHANGE_TYPE','PRODUCTS','PARTNER_TYPE','ROUTING_METHOD','DECISION_CHANGED'];
const lines=(value:string)=>value.split(/[\n,]/).map(s=>s.trim()).filter(Boolean);

/** Keep the exact submitted JSON visible; the guided controls preserve unknown settings. */
export default function SettingsEditor({initial}:{initial:Row}) {
 const [text,setText]=useState(()=>JSON.stringify(initial,null,2));
 let config:Row={};let parseError='';
 try {config=JSON.parse(text);if(!config||Array.isArray(config)||typeof config!=='object')throw Error('Ожидается JSON-объект.');}
 catch {parseError='Исправьте JSON ниже, чтобы продолжить настройку.';config={};}
 if(!parseError&&(!Array.isArray(config.exclude_paths||[])||!Array.isArray(config.aggregate_dimensions||[])||typeof (config.array_rules||{})!=='object'||Object.values(config.array_rules||{}).some((rule:any)=>!rule||typeof rule!=='object'||!Array.isArray(rule.fields||[]))||typeof (config.bucket_rules||{})!=='object')){parseError='Проверьте структуру настроек: пути и измерения — списки, правила — объекты.';config={};}
 const update=(patch:Row)=>setText(JSON.stringify({...config,...patch},null,2));
 const arrays=Object.entries(config.array_rules||{}) as [string,Row][];
 const buckets=Object.entries(config.bucket_rules||{}) as [string,unknown][];
 function setArray(index:number,path:string,rule:Row){const next=arrays.map(([p,r],i)=>i===index?[path,rules(rule)]:[p,r]);update({array_rules:Object.fromEntries(next)});}
 function rules(rule:Row){return rule.mode==='field'?{...rule,fields:rule.fields||[]}:Object.fromEntries(Object.entries(rule).filter(([key])=>key!=='fields'));}
 return <div className="settings-editor">
  <details><summary>Исключения и сопоставление массивов</summary>
   {!parseError&&<>
    <label className="field">Исключаемые пути<span className="help">Один путь на строку. Поддерживаются маски со знаком *.</span><textarea rows={3} value={(config.exclude_paths||[]).join('\n')} onChange={e=>update({exclude_paths:e.target.value.split('\n')})}/></label>
    <div className="settings-list">{arrays.map(([path,rule],i)=><div className="settings-row" key={i}>
     <label className="field">Путь массива<input aria-label={'Путь массива '+(i+1)} value={path} onChange={e=>setArray(i,e.target.value,rule)}/></label>
     <label className="field">Сопоставление<select value={rule.mode} onChange={e=>setArray(i,path,{...rule,mode:e.target.value})}><option value="field">По полям (BY_FIELD)</option><option value="value">По значению (BY_VALUE)</option><option value="index">По позиции (BY_INDEX)</option></select></label>
     {rule.mode==='field'&&<label className="field">Поля ключа<input value={(rule.fields||[]).join(', ')} onChange={e=>setArray(i,path,{...rule,fields:e.target.value.split(',').map((s:string)=>s.trim())})} placeholder="product_id, type"/></label>}
     <button type="button" className="ghost small" aria-label={'Удалить массив '+path} onClick={()=>update({array_rules:Object.fromEntries(arrays.filter((_,n)=>i!==n))})}>Удалить</button>
    </div>)}</div>
    <button type="button" className="secondary small" onClick={()=>update({array_rules:{...config.array_rules,['new_array_'+(arrays.length+1)]:{mode:'field',fields:['id']}}})}>＋ Правило массива</button>
   </>}
  </details>
  <details><summary>Бакеты и измерения агрегации</summary>
   {!parseError&&<>
    <p className="tiny muted">Бакеты группируют значения в фактах. Сравнение использует исходные значения.</p>
    {buckets.map(([path,bounds],i)=><div className="bucket-row" key={i}>
     <label className="field">Маска пути<input value={path} onChange={e=>update({bucket_rules:Object.fromEntries(buckets.map(([p,v],n)=>[n===i?e.target.value:p,v]))})}/></label>
     <label className="field">Границы (JSON)<input defaultValue={JSON.stringify(bounds)} key={path} onBlur={e=>{try{const value=JSON.parse(e.target.value);if(!Array.isArray(value)||value.some(x=>typeof x!=='number'))throw Error();update({bucket_rules:{...config.bucket_rules,[path]:value}});e.target.setCustomValidity('');}catch{e.target.setCustomValidity('Укажите массив чисел, например [0, 0.3, 0.6, 1]');e.target.reportValidity();}}}/></label>
     <button type="button" className="ghost small" aria-label={'Удалить бакет '+path} onClick={()=>update({bucket_rules:Object.fromEntries(buckets.filter((_,n)=>n!==i))})}>Удалить</button>
    </div>)}
    <button type="button" className="secondary small" onClick={()=>update({bucket_rules:{...config.bucket_rules,['new_bucket_'+(buckets.length+1)]:[0,1]}})}>＋ Правило бакетов</button>
    <div className="section-label">Группировать по</div><div className="dimension-options">{dimensions.map(d=><label className="checkbox" key={d}><input type="checkbox" checked={(config.aggregate_dimensions||[]).includes(d)} onChange={e=>update({aggregate_dimensions:e.target.checked?[...(config.aggregate_dimensions||[]),d]:(config.aggregate_dimensions||[]).filter((v:string)=>v!==d)})}/>{d}</label>)}</div>
    <label className="field">Полный список измерений<span className="help">Можно добавить свои поля разрезов через запятую.</span><input key={(config.aggregate_dimensions||[]).join(',')} defaultValue={(config.aggregate_dimensions||[]).join(', ')} onBlur={e=>update({aggregate_dimensions:lines(e.target.value)})}/></label>
   </>}
  </details>
  {parseError&&<p className="error-box" role="alert">{parseError}</p>}
  <details open={!!parseError}><summary>Все настройки JSON</summary><label className="field">Настройки запуска<textarea name="settings" className="mono" rows={16} spellCheck={false} value={text} onChange={e=>setText(e.target.value)}/><span className="help">Здесь также доступны лимиты обработки и параметры Python worker. Перед сохранением JSON проверяется.</span></label></details>
 </div>;
}
