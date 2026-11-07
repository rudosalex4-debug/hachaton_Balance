import {createContext,useContext,useEffect,useState,type ReactNode} from 'react';

type Theme = 'light' | 'dark';
const storageKey = 'balance-platform-theme';
const ThemeContext = createContext<{theme:Theme;toggle:()=>void}>({theme:'light',toggle:()=>{}});

export function ThemeProvider({children}:{children:ReactNode}) {
  const [theme,setTheme] = useState<Theme>(()=>document.documentElement.dataset.theme==='dark'?'dark':'light');
  useEffect(()=>{
    document.documentElement.dataset.theme=theme;
    document.querySelector('meta[name="theme-color"]')?.setAttribute('content',theme==='dark'?'#1b1c1e':'#f7f8fa');
    try {localStorage.setItem(storageKey,theme);} catch {/* Switching still works when browser storage is unavailable. */}
  },[theme]);
  useEffect(()=>{
    const sync=(event:StorageEvent)=>{
      if(event.key===storageKey||event.key===null)setTheme(event.newValue==='dark'?'dark':'light');
    };
    window.addEventListener('storage',sync);
    return()=>window.removeEventListener('storage',sync);
  },[]);
  return <ThemeContext.Provider value={{theme,toggle:()=>setTheme(current=>current==='light'?'dark':'light')}}>{children}</ThemeContext.Provider>;
}

export function useTheme(){return useContext(ThemeContext);}

export function ThemeToggle(){
  const {theme,toggle}=useTheme(),dark=theme==='dark';
  return <button type="button" className="secondary small theme-toggle" role="switch" aria-label="Тёмная тема" aria-checked={dark} title={dark?'Включить светлую тему':'Включить тёмную тему'} onClick={toggle}>
    <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      {dark?<path d="M20.5 13.1A8.6 8.6 0 0 1 10.9 3.5 8.6 8.6 0 1 0 20.5 13.1Z"/>:<><circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5"/></>}
    </svg>
  </button>;
}
