import type {FormEvent, ReactNode} from 'react';
import './auth.css';
import {BrandMark} from './Icons';

type AuthScreenProps = {
  register: boolean;
  pending: boolean;
  error: ReactNode;
  onToggle: () => void;
  onSubmit: (event: FormEvent<HTMLFormElement>) => void;
};

function ComparisonGraphic() {
  return <div className="access-graphic" aria-hidden="true">
    <div className="access-orbit"/>
    <div className="access-data-panel access-data-before">
      <div className="access-data-label"><span className="access-data-dot"/>BEFORE <b>MASTER</b></div>
      <div className="access-data-grid">{Array.from({length:28}, (_, i) => <i key={i} className={i % 5 === 0 ? 'highlight' : ''}/>)}</div>
      <div className="access-data-bottom"><span>01 / ИСХОДНЫЙ СРЕЗ</span><span>↗</span></div>
    </div>
    <div className="access-data-panel access-data-after">
      <div className="access-data-label"><span className="access-data-dot"/>AFTER <b>BETA</b></div>
      <div className="access-data-grid">{Array.from({length:28}, (_, i) => <i key={i} className={[2,5,9,10,16,20,23,26].includes(i) ? 'highlight' : ''}/>)}</div>
      <div className="access-data-bottom"><span>02 / НОВЫЙ СРЕЗ</span><span>↗</span></div>
    </div>
    <div className="access-compare-mark">⇄</div>
  </div>;
}

export default function AuthScreen({register,pending,error,onToggle,onSubmit}: AuthScreenProps) {
  return <main className="access-page">
    <header className="access-header">
      <div className="access-brand"><span className="access-brand-symbol" aria-hidden="true"><BrandMark/></span><span>balance platform<small>АНАЛИТИКА РЕШЕНИЙ</small></span></div>
      <span className="access-header-note">Сравнение результатов СПР</span>
    </header>
    <div className="access-layout">
      <section className="access-story" aria-labelledby="access-title">
        <div className="access-eyebrow"><span/>ОБРАБОТКА РЕЗУЛЬТАТОВ СПР</div>
        <h1 id="access-title">Сравнение<br/><em>решений СПР</em></h1>
        <p className="access-description">Загрузите срезы MASTER и BETA, настройте обработку и изучите изменения атрибутов, решений и результатов проверок.</p>
        <ComparisonGraphic/>
        <div className="access-stages"><span><b>01</b> Приведение</span><i>→</i><span><b>02</b> Сравнение</span><i>→</i><span><b>03</b> Агрегация</span><i>→</i><span><b>04</b> Проверки</span></div>
      </section>
      <section className="access-form-side" aria-labelledby="access-form-title">
        <div className="access-form-card">
          <div className="access-form-icon" aria-hidden="true">↗</div>
          <div className="access-eyebrow">РАБОЧЕЕ ПРОСТРАНСТВО</div>
          <h2 id="access-form-title">{register?'Регистрация':'Вход'}</h2>
          <p className="access-form-description">{register?'Задайте имя пользователя и пароль.':'Укажите имя пользователя и пароль.'}</p>
          <form onSubmit={onSubmit}>
            <label className="access-field">Имя пользователя<input name="username" autoComplete="username" required minLength={2} maxLength={64} pattern="[A-Za-z0-9_.@\-]+" title="Латинские буквы, цифры и символы _.@-" placeholder="analyst"/></label>
            <label className="access-field">Пароль<input name="password" type="password" autoComplete={register?'new-password':'current-password'} required minLength={8} maxLength={256} placeholder="Не менее 8 символов"/></label>
            {error}
            <button className="access-submit" type="submit" disabled={pending}>{pending?(register?'Создание аккаунта…':'Вход…'):register?'Создать аккаунт →':'Войти →'}</button>
            <div className="access-toggle"><button type="button" onClick={onToggle}>{register?'Вход в аккаунт':'Регистрация'}</button></div>
          </form>
        </div>
      </section>
    </div>
    <footer className="access-footer"><span>balance platform</span><span>MASTER → BETA</span></footer>
  </main>;
}
