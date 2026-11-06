export type Row = Record<string, any>;
export class ApiError extends Error { constructor(message:string,public status:number){super(message);} }
export async function request<T=Row>(path:string,method='GET',body?:unknown):Promise<T>{
  let response:Response;
  try{response=await fetch('/api/v1'+path,{method,credentials:'same-origin',headers:body===undefined?{}:{'Content-Type':'application/json'},body:body===undefined?undefined:JSON.stringify(body)});}
  catch{throw new ApiError('Не удалось связаться с сервером. Проверьте подключение и повторите действие.',0);}
  const text=await response.text();let value:Row;
  try{value=JSON.parse(text);}catch{throw new ApiError(response.status===413?'Размер загрузки превышает ограничение сервера. Используйте подключение серверной папки.':`Сервер вернул некорректный ответ (HTTP ${response.status}). Повторите запрос позже.`,response.status);}
  if(!response.ok){if(response.status===401&&!['/login','/register'].includes(path))window.dispatchEvent(new Event('session-expired'));throw new ApiError(String(value.error?.message||value.error||value.message||`HTTP ${response.status}`),response.status);}
  return value as T;
}
export function list(value:Row|undefined,key:string):Row[]{return value?.items||value?.[key]||[];}
export function numeric(value:unknown):string{return new Intl.NumberFormat('ru-RU',{maximumFractionDigits:2}).format(Number(value)||0);}
export function print(value:unknown):string{return value===undefined||value===null?'—':typeof value==='object'?JSON.stringify(value):String(value);}
export function timestamp(value:string):string{const date=new Date(value);return value&&Number.isFinite(date.getTime())?date.toLocaleString('ru-RU',{day:'2-digit',month:'short',hour:'2-digit',minute:'2-digit'}):'—';}
export function status(value:string):string{return (value||'queued').toLowerCase();}
export function active(value:string):boolean{return !['completed','failed','cancelled'].includes(status(value));}
export function getRoute(){
 const [hashPath,hashSearch]=(location.hash.slice(1)).split('?');
 const search=new URLSearchParams(location.search),run=search.get('run');
 const path=hashPath||(run?`run/${encodeURIComponent(run)}/${search.get('tab')||'report'}`:'runs');
 const params=hashPath?new URLSearchParams(hashSearch):search;params.delete('run');params.delete('tab');
 if(params.has('app')&&!params.has('app_id'))params.set('app_id',params.get('app')||'');params.delete('app');
 return {parts:path.split('/').map(part=>{try{return decodeURIComponent(part);}catch{return part;}}),params};
}
export function navigate(path:string,params?:Record<string,unknown>){const q=new URLSearchParams();Object.entries(params||{}).forEach(([k,v])=>{if(v!==undefined&&v!==null&&v!=='')q.set(k,String(v));});location.hash=path+(q.size?'?'+q:'');}
