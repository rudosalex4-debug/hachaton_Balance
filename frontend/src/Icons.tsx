export function BrandMark() {
  return <svg viewBox="0 0 32 32" fill="none" aria-hidden="true"><path d="m16 3 12 13-12 13L4 16 16 3Z" stroke="currentColor" strokeWidth="2"/><path d="m16 10 6 6-6 6-6-6 6-6Z" fill="currentColor"/></svg>;
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
