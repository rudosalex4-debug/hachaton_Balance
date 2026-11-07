import {useEffect,useRef,useState} from 'react';

export default function AccountMenu({username,pending,onLogout}:{username:string;pending:boolean;onLogout:()=>void}) {
  const [open,setOpen]=useState(false),container=useRef<HTMLDivElement>(null),trigger=useRef<HTMLButtonElement>(null),logout=useRef<HTMLButtonElement>(null);
  useEffect(()=>{
    if(!open)return;
    logout.current?.focus();
    const outside=(event:PointerEvent)=>{if(!container.current?.contains(event.target as Node))setOpen(false);};
    const escape=(event:KeyboardEvent)=>{if(event.key==='Escape'){setOpen(false);trigger.current?.focus();}};
    document.addEventListener('pointerdown',outside);
    document.addEventListener('keydown',escape);
    return()=>{document.removeEventListener('pointerdown',outside);document.removeEventListener('keydown',escape);};
  },[open]);
  return <div className="account-menu" ref={container} onBlur={event=>{if(!event.currentTarget.contains(event.relatedTarget))setOpen(false);}}>
    <button ref={trigger} type="button" className="account-button" aria-label="Аккаунт" title={username} aria-expanded={open} aria-haspopup="dialog" aria-controls={open?'account-menu':undefined} onClick={()=>setOpen(!open)}>
      <svg viewBox="0 0 24 24" width="21" height="21" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" aria-hidden="true"><circle cx="12" cy="8" r="3.5"/><path d="M5 20v-2a7 7 0 0 1 14 0v2"/></svg>
    </button>
    {open&&<div id="account-menu" className="account-popover" role="dialog" aria-label="Аккаунт">
      <strong>{username}</strong>
      <button ref={logout} className="secondary small" type="button" disabled={pending} onClick={onLogout}>{pending?'Выход…':'Выйти'}</button>
    </div>}
  </div>;
}
