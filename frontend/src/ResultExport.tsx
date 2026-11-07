import {useState} from 'react';
import {useMutation} from '@tanstack/react-query';
import {ApiError} from './api';

const mediaTypes = {
  json: ['application/json'],
  xml: ['application/xml', 'text/xml'],
  csv: ['text/csv'],
  html: ['text/html']
};
type Format = keyof typeof mediaTypes;

async function saveAttachment(url: string, filename: string, format: Format) {
  let response: Response;
  try {
    response = await fetch(url, {credentials: 'same-origin'});
  } catch {
    throw new Error('Не удалось связаться с сервером для выгрузки.');
  }
  if (!response.ok) {
    if (response.status === 401) window.dispatchEvent(new Event('session-expired'));
    const detail = await response.json().catch(() => null);
    throw new ApiError(detail?.error?.message || detail?.error || `Ошибка выгрузки (HTTP ${response.status}).`, response.status);
  }
  const mediaType = response.headers.get('content-type')?.split(';')[0].trim().toLowerCase() || '';
  const disposition = response.headers.get('content-disposition') || '';
  if (!/^attachment(?:\s*;|$)/i.test(disposition) || !mediaTypes[format].includes(mediaType)) {
    throw new Error('Сервер вернул ответ в неожиданном формате. Повторите выгрузку.');
  }
  const attachment = await response.blob();
  const link = document.createElement('a');
  link.href = URL.createObjectURL(attachment);
  link.download = filename;
  document.body.append(link);
  link.click();
  link.remove();
  window.setTimeout(() => URL.revokeObjectURL(link.href), 1000);
}

export function HtmlDownload({runId}: {runId: string}) {
  const url = `/api/v1/runs/${encodeURIComponent(runId)}/report.html`;
  const filename = `balance-platform-${runId}.html`;
  const download = useMutation({mutationFn: () => saveAttachment(url, filename, 'html')});
  return <><a className="button" aria-disabled={download.isPending} href={url} download={filename} onClick={event => {
    event.preventDefault();
    if (!download.isPending) download.mutate();
  }}>{download.isPending ? 'Загрузка HTML…' : 'Скачать HTML ↓'}</a>
    {download.error && <div className="error-box" role="alert">{download.error.message}</div>}</>;
}

export function ResultExport({runId}: {runId: string}) {
  const [artifact, setArtifact] = useState('comparisons');
  const [format, setFormat] = useState<Exclude<Format, 'html'>>('json');
  const download = useMutation({mutationFn: () => {
    const query = new URLSearchParams({artifact, format});
    const url = `/api/v1/runs/${encodeURIComponent(runId)}/export?${query}`;
    return saveAttachment(url, `balance-platform-${runId}-${artifact}.${format}`, format);
  }});
  return <details className="card result-export">
    <summary>Выгрузить таблицу целиком</summary>
    <form className="toolbar" onSubmit={event => {event.preventDefault(); download.mutate();}}>
      <label className="field">Таблица<select aria-label="Таблица для выгрузки" value={artifact} disabled={download.isPending} onChange={e => {setArtifact(e.target.value); download.reset();}}>
        <option value="normalized">Нормализованные атрибуты</option>
        <option value="comparisons">Сопоставленные значения</option>
        <option value="facts">Агрегированные факты</option>
        <option value="checks">Автоматические проверки</option>
      </select></label>
      <label className="field">Формат<select aria-label="Формат выгрузки" value={format} disabled={download.isPending} onChange={e => {setFormat(e.target.value as typeof format); download.reset();}}>
        <option value="json">JSON</option><option value="xml">XML</option><option value="csv">CSV</option>
      </select></label>
      <button type="submit" disabled={download.isPending}>{download.isPending ? 'Подготовка файла…' : 'Скачать таблицу'}</button>
    </form>
    {download.error && <div className="error-box" role="alert">{download.error.message}</div>}
  </details>;
}
