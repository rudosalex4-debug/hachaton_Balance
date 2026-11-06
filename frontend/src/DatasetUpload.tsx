import {useRef,useState} from 'react';
import {useMutation,useQueryClient} from '@tanstack/react-query';
import {request,numeric} from './api';

export default function DatasetUpload(){
 const client=useQueryClient(),input=useRef<HTMLInputElement>(null);
 const [files,setFiles]=useState<File[]>([]),[name,setName]=useState(''),[reading,setReading]=useState(0);
 const mutation=useMutation({mutationFn:async()=>{
  const selected=files.filter(file=>file.name.endsWith('.json'));
  if(!selected.length)throw Error('Выберите папку с manifest.json и RESULTS.');
  if(selected.length>10001)throw Error('Максимум 10 001 JSON-файл в одном наборе.');
  if(selected.reduce((sum,file)=>sum+file.size,0)>48*1024*1024)throw Error('Папка больше 48 МиБ. Подключите её по серверному пути ниже.');
  const records=[];setReading(0);
  for(const [index,file] of selected.entries()){
   records.push({path:file.webkitRelativePath||file.name,content:await file.text()});setReading(index+1);
  }
  const body={name:name.trim()||selected[0].webkitRelativePath.split('/')[0],files:records};
  if(new Blob([JSON.stringify(body)]).size>64*1024*1024)throw Error('Размер запроса больше 64 МиБ. Подключите папку по серверному пути.');
  return request('/datasets/upload','POST',body);
 },onSuccess:()=>client.invalidateQueries({queryKey:['/datasets']})});
 return <section className="card"><div className="card-head"><div><h2>Загрузить папку с компьютера</h2><p className="subtitle">Выберите папку запуска с manifest.json и RESULTS. Сервер проверит JSON и пары заявок.</p></div></div>
  <form className="card-body" onSubmit={event=>{event.preventDefault();mutation.mutate();}}>
   <div className="form-grid"><label className="field">Название набора<input value={name} onChange={e=>setName(e.target.value)} placeholder="Прогон стратегии" disabled={mutation.isPending}/></label>
   <label className="field">Папка запуска<input ref={element=>{input.current=element;element?.setAttribute('webkitdirectory','');}} type="file" multiple disabled={mutation.isPending} onChange={e=>{const next=Array.from(e.target.files||[]);setFiles(next);setReading(0);mutation.reset();if(!name&&next[0])setName(next[0].webkitRelativePath.split('/')[0]);}}/><span className="help">До 48 МиБ исходных JSON; для больших наборов используйте серверный путь.</span></label></div>
   {files.length>0&&<p className="tiny muted">{numeric(files.filter(f=>f.name.endsWith('.json')).length)} JSON-файлов · {numeric(files.reduce((sum,file)=>sum+file.size,0)/1048576)} МиБ</p>}
   {mutation.error&&<div role="alert" className="error-box">{mutation.error.message}</div>}
   {mutation.isSuccess&&<p role="status" className="inline-status">Набор загружен и проверен. Его можно выбрать в конфигурации запуска.</p>}
   <button disabled={!files.length||mutation.isPending}>{mutation.isPending?(reading<files.filter(f=>f.name.endsWith('.json')).length?`Читаем файлы: ${numeric(reading)}…`:'Загружаем и проверяем…'):'Загрузить и проверить'}</button>
  </form>
 </section>;
}
