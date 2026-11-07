export function BrandMark() {
  return <svg viewBox="0 0 40 40" fill="none" aria-hidden="true">
    <path d="M9 14C4 12 4 5 9 5c3 0 5 2 6 5h10c1-3 3-5 6-5 5 0 5 7 0 9" fill="var(--accent, #6cdb57)" stroke="currentColor" strokeWidth="1.8"/>
    <path d="M9 11c-2-2-1-4 1-3l2 3m16 0 2-3c2-1 3 1 1 3" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"/>
    <path d="M20 8C10 8 5 14 5 23c0 8 7 13 15 14 8-1 15-6 15-14 0-9-5-15-15-15Z" fill="var(--accent, #6cdb57)" stroke="currentColor" strokeWidth="1.8"/>
    <path d="m17 9 3 8 3-8M9 14l7 4-3-6m18 2-7 4 3-6M5 20l7 3-7 2m30-5-7 3 7 2M7 29l6-2-3 6m23-4-6-2 3 6" fill="currentColor"/>
    <path d="M13 27c0-3 4-5 7-3 3-2 7 0 7 3 0 5-4 8-7 8s-7-3-7-8Z" fill="#e9f9e4"/>
    <path d="m11.5 20 4 1m9 0 4-1" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
    <path d="m17 26 3 3 3-3c-1-2-5-2-6 0Z" fill="currentColor"/>
    <path d="M20 29v2m-4 0c1 2 3 2 4 0 1 2 3 2 4 0" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round"/>
  </svg>;
}

export function NavIcon({name}:{name:string}) {
  const paths:Record<string,React.ReactNode>={
    runs:<><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></>,
    configs:<><path d="M4 7h16M4 17h16"/><circle cx="9" cy="7" r="3" fill="var(--bg)"/><circle cx="15" cy="17" r="3" fill="var(--bg)"/></>,
    datasets:<><ellipse cx="12" cy="5" rx="8" ry="3"/><path d="M4 5v14c0 4 16 4 16 0V5M4 12c0 4 16 4 16 0"/></>,
    procedures:<><path d="m7 6-5 6 5 6m10-12 5 6-5 6M14 3l-4 18"/></>,
  };
  return <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">{paths[name]}</svg>;
}
