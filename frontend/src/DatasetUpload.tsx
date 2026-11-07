import {useMemo, useState} from 'react';
import {useMutation, useQueryClient} from '@tanstack/react-query';
import {request, numeric} from './api';

const MiB = 1024 * 1024;
const supported = /\.(json|xml|csv)$/i;

function isDatasetFile(file: File) {
  const parts = file.webkitRelativePath.split('/').slice(1);
  if (parts.length === 1) return parts[0] === 'manifest.json';
  return parts.length === 3 && parts[0] === 'RESULTS'
    && (parts[1] === 'BEFORE_MASTER' || parts[1] === 'AFTER_BETA')
    && supported.test(parts[2]);
}

async function readSource(file: File) {
  const bytes = await file.arrayBuffer();
  try {
    return new TextDecoder('utf-8', {fatal: true, ignoreBOM: true}).decode(bytes);
  } catch {
    throw new Error(`Сохраните ${file.name} в кодировке UTF-8 и повторите загрузку.`);
  }
}

export default function DatasetUpload() {
  const client = useQueryClient();
  const [files, setFiles] = useState<File[]>([]);
  const [name, setName] = useState('');
  const [reading, setReading] = useState(0);
  const bytes = useMemo(() => files.reduce((sum, file) => sum + file.size, 0), [files]);
  const mutation = useMutation({
    mutationFn: async () => {
      if (!files.some(file => file.name === 'manifest.json')) {
        throw new Error('Выберите корневую папку с manifest.json и каталогом RESULTS.');
      }
      if (files.length > 10001) throw new Error('Максимум 10 001 файл в одном наборе.');
      if (bytes > 48 * MiB) throw new Error('Папка больше 48 МиБ. Подключите её по серверному пути.');
      const records = [];
      setReading(0);
      for (const [index, file] of files.entries()) {
        if (file.size > 8 * MiB) throw new Error(`Файл ${file.name} больше 8 МиБ.`);
        records.push({path: file.webkitRelativePath || file.name, content: await readSource(file)});
        if ((index + 1) % 16 === 0 || index + 1 === files.length) setReading(index + 1);
      }
      const body = {name: name.trim() || files[0].webkitRelativePath.split('/')[0], files: records};
      if (new Blob([JSON.stringify(body)]).size > 64 * MiB) {
        throw new Error('Размер запроса больше 64 МиБ. Подключите папку по серверному пути.');
      }
      return request('/datasets/upload', 'POST', body);
    },
    onSuccess: () => client.invalidateQueries({queryKey: ['/datasets']})
  });
  return <section className="card">
    <div className="card-head"><div><h2>Загрузить папку с компьютера</h2>
      <p className="subtitle">Выберите папку с manifest.json и каталогом RESULTS. Срезы могут содержать ответы в разных форматах. Профили XML и CSV задаются в поле input_profiles манифеста.</p>
    </div></div>
    <form className="card-body" onSubmit={event => {event.preventDefault(); mutation.mutate();}}>
      <div className="form-grid">
        <label className="field">Название набора<input value={name} onChange={e => setName(e.target.value)} placeholder="Прогон стратегии" disabled={mutation.isPending}/></label>
        <label className="field">Папка запуска<input type="file" multiple ref={element => element?.setAttribute('webkitdirectory', '')} disabled={mutation.isPending} onChange={event => {
          const items = Array.from(event.target.files || []);
          setFiles(items.filter(isDatasetFile));
          setReading(0);
          mutation.reset();
          if (!name && items.length) setName(items[0].webkitRelativePath.split('/')[0]);
        }}/><span className="help">JSON, XML или CSV в UTF-8. До 8 МиБ на файл и 48 МиБ на набор.</span></label>
      </div>
      {!!files.length && <p className="tiny muted">Файлов к загрузке: {numeric(files.length)} | {numeric(bytes / MiB)} МиБ</p>}
      {mutation.error && <div role="alert" className="error-box">{mutation.error.message}</div>}
      {mutation.isSuccess && <p role="status" className="inline-status">Набор загружен и проверен. Его можно выбрать в конфигурации запуска.</p>}
      <button disabled={!files.length || mutation.isPending}>{mutation.isPending
        ? (reading < files.length ? `Чтение файлов (${numeric(reading)} из ${numeric(files.length)})…` : 'Загрузка и проверка…')
        : 'Загрузить и проверить'}</button>
    </form>
  </section>;
}
