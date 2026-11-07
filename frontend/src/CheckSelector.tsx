import {useState} from 'react';
import type {Row} from './api';

export default function CheckSelector({procedures,existing}:{procedures:Row[];existing?:Row}){
 const available=procedures.filter(p=>p.kind==='check');
 const [selected,setSelected]=useState<{procedure_id:string;params_text:string}[]>(()=>{
  const initial=existing?(existing.checks||[]):available.filter(p=>!p.owner).map(p=>({procedure_id:p.id,params:p.params||{}}));
  return initial.map((item:Row)=>({procedure_id:item.procedure_id,params_text:JSON.stringify(item.params||{},null,2)}));
 });
 function toggle(p:Row,checked:boolean){setSelected(current=>checked?[...current,{procedure_id:p.id,params_text:JSON.stringify(p.params||{},null,2)}]:current.filter(c=>c.procedure_id!==p.id));}
 return <section className="check-selection"><h3>Автоматические проверки</h3><p className="tiny muted">Проверки выполняются после агрегации. Настройте пороги для этого запуска; результат появится в едином отчёте.</p>
  <input type="hidden" name="check_settings" value={JSON.stringify(selected)}/>
  {available.length===0?<p className="note">Проверок пока нет. Создайте Python-процедуру с типом «Проверка».</p>:available.map(p=>{const item=selected.find(c=>c.procedure_id===p.id);return <div className="check-option" key={p.id}><label className="checkbox"><input type="checkbox" checked={!!item} onChange={e=>toggle(p,e.target.checked)}/><span>{p.name}<small className="muted">{p.owner?'  |  пользовательская':'  |  стандартная'}</small></span></label>{item&&<details><summary>Параметры проверки</summary><label className="field">Параметры {p.name}<textarea className="mono" rows={5} spellCheck={false} value={item.params_text} onChange={e=>setSelected(current=>current.map(c=>c.procedure_id===p.id?{...c,params_text:e.target.value}:c))}/><span className="help">JSON-объект. app_ids_limit ограничивает примеры заявок; low_confidence_cnt задаёт порог слабого сигнала.</span></label></details>}</div>;})}
  {selected.some(c=>!available.some(p=>p.id===c.procedure_id))&&<p role="alert" className="error-box">В конфигурации есть недоступная проверка. Удалите её из выбора: {selected.filter(c=>!available.some(p=>p.id===c.procedure_id)).map(c=><button type="button" key={c.procedure_id} onClick={()=>setSelected(s=>s.filter(x=>x!==c))}>{c.procedure_id} ×</button>)}</p>}
  <p className="tiny muted">Выбрано: {selected.length}. <a href="#procedures/new">Добавить свою проверку →</a></p>
 </section>;
}
