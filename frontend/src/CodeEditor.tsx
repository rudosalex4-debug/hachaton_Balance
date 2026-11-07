import {useEffect,useRef} from 'react';
import * as monaco from 'monaco-editor/esm/vs/editor/editor.api';
import 'monaco-editor/esm/vs/basic-languages/python/python.contribution';
import EditorWorker from 'monaco-editor/esm/vs/editor/editor.worker?worker';

(self as unknown as {MonacoEnvironment: {getWorker:()=>Worker}}).MonacoEnvironment={getWorker:()=>new EditorWorker()};
export default function CodeEditor({value,onChange,readOnly=false}:{value:string;onChange:(value:string)=>void;readOnly?:boolean}){
 const element=useRef<HTMLDivElement>(null),editor=useRef<monaco.editor.IStandaloneCodeEditor|null>(null),callback=useRef(onChange);
 callback.current=onChange;
 useEffect(()=>{
  if(!element.current)return;
  monaco.editor.defineTheme('balance-platform',{
   base:'vs',inherit:true,
   rules:[{token:'keyword',foreground:'783DD0'},{token:'string',foreground:'287C1B'},{token:'number',foreground:'975809'},{token:'comment',foreground:'727984'}],
   colors:{'editor.background':'#FFFFFF','editor.foreground':'#20232B','editorLineNumber.foreground':'#727984','editorLineNumber.activeForeground':'#287C1B','editorCursor.foreground':'#287C1B','editor.selectionBackground':'#DCCAF666','editor.lineHighlightBackground':'#F7F8FA'}
  });
  const model=monaco.editor.createModel(value,'python');
  const instance=monaco.editor.create(element.current,{model,theme:'balance-platform',automaticLayout:true,minimap:{enabled:false},fontSize:13,lineHeight:21,scrollBeyondLastLine:false,readOnly,tabSize:4,wordWrap:'off',padding:{top:14},ariaLabel:'Редактор процедуры Python'});
  editor.current=instance;
  const change=model.onDidChangeContent(()=>callback.current(model.getValue()));
  return()=>{change.dispose();instance.dispose();model.dispose();editor.current=null;};
 },[]);
 useEffect(()=>{if(editor.current&&editor.current.getValue()!==value)editor.current.setValue(value);},[value]);
 useEffect(()=>{editor.current?.updateOptions({readOnly});},[readOnly]);
 return <div ref={element} className="code-editor"/>;
}
