// import { useState, useEffect, useRef, useCallback } from "react";
// import { AreaChart, Area, LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer, BarChart, Bar } from "recharts";
// import { Database, Search, Activity, Server, Zap, AlertTriangle, CheckCircle, RefreshCw, Play, ChevronRight, Layers, Cpu, Settings } from "lucide-react";

// // ── Types ─────────────────────────────────────────────────────────────────────

// interface ShardInfo { id: number; vectors: number; status: "healthy"|"degraded"|"offline"; }

// interface MetricsSnapshot {
//   ts: number; totalVectors: number;
//   insertRate: number; searchRate: number;
//   p50Us: number; p95Us: number; p99Us: number;
//   totalInserts: number; totalSearches: number;
//   shards: ShardInfo[];
// }

// interface SearchResult { nodeId: number; distance: number; }

// interface SearchResponse {
//   latency_us: number; degraded: boolean;
//   shards_ok: number; shards_err: number;
//   results: SearchResult[];
// }

// // ── API ───────────────────────────────────────────────────────────────────────

// async function fetchMetrics(base: string): Promise<MetricsSnapshot | null> {
//   try {
//     const r = await fetch(`${base}/api/metrics`, { signal: AbortSignal.timeout(2000) });
//     if (!r.ok) return null;
//     const d = await r.json();
//     return {
//       ts: d.ts, totalVectors: d.total_vectors,
//       insertRate: d.insert_rate, searchRate: d.search_rate,
//       p50Us: d.p50_us, p95Us: d.p95_us, p99Us: d.p99_us,
//       totalInserts: d.total_inserts, totalSearches: d.total_searches,
//       shards: (d.shards||[]).map((s:{id:number;vectors:number;status:string}) => ({
//         id: s.id, vectors: s.vectors, status: s.status as "healthy"|"degraded"|"offline"
//       })),
//     };
//   } catch { return null; }
// }

// async function apiSearch(base: string, query: number[], topK: number): Promise<SearchResponse> {
//   const r = await fetch(`${base}/api/search`, {
//     method: "POST",
//     headers: { "Content-Type": "application/json" },
//     body: JSON.stringify({ query, top_k: topK }),
//     signal: AbortSignal.timeout(5000),
//   });
//   const d = await r.json();
//   if (d.error) throw new Error(d.error);
//   return {
//     latency_us: d.latency_us, degraded: d.degraded,
//     shards_ok: d.shards_ok, shards_err: d.shards_err,
//     results: (d.results||[]).map((r:{node_id:number;distance:number}) => ({ nodeId: r.node_id, distance: r.distance })),
//   };
// }

// // ── Live metrics hook (real polling every 1s) ─────────────────────────────────

// function useLiveMetrics(serverBase: string, connected: boolean) {
//   const [history, setHistory] = useState<MetricsSnapshot[]>([]);
//   const [latest, setLatest]   = useState<MetricsSnapshot | null>(null);
//   const [connError, setConnError] = useState("");
//   const ivRef = useRef<ReturnType<typeof setInterval> | null>(null);

//   const poll = useCallback(async () => {
//     const snap = await fetchMetrics(serverBase);
//     if (!snap) { setConnError("Cannot reach server — is it running on port 8080?"); return; }
//     setConnError("");
//     setLatest(snap);
//     setHistory(h => [...h.slice(-59), snap]);
//   }, [serverBase]);

//   useEffect(() => {
//     if (!connected) { if (ivRef.current) clearInterval(ivRef.current); setHistory([]); setLatest(null); return; }
//     poll();
//     ivRef.current = setInterval(poll, 1000);
//     return () => { if (ivRef.current) clearInterval(ivRef.current); };
//   }, [connected, poll]);

//   return { history, latest, connError };
// }

// // ── Design tokens ─────────────────────────────────────────────────────────────

// const C = {
//   bg:"#0A0D12", surface:"#111520", border:"#1E2535",
//   accent:"#00D4FF", accentDim:"#0099BB",
//   green:"#00E5A0", amber:"#FFB020", red:"#FF4560",
//   text:"#E8EDF5", muted:"#5A6478",
//   c1:"#00D4FF", c2:"#00E5A0", c3:"#7C5CFF",
// };

// // ── Helpers ───────────────────────────────────────────────────────────────────

// function fmt(n:number, dec=0) {
//   if (n>=1_000_000) return (n/1_000_000).toFixed(1)+"M";
//   if (n>=1_000) return (n/1_000).toFixed(dec)+"K";
//   return n.toFixed(dec);
// }
// function fmtUs(us:number) {
//   if (us===0) return "—";
//   if (us>=1000) return (us/1000).toFixed(1)+" ms";
//   return us.toFixed(0)+" µs";
// }

// // ── Components ────────────────────────────────────────────────────────────────

// function StatCard({icon:Icon,label,value,sub,accent=false}:{icon:React.ElementType;label:string;value:string;sub?:string;accent?:boolean}) {
//   return (
//     <div style={{background:C.surface,border:`1px solid ${accent?C.accent+"44":C.border}`,borderRadius:12,padding:"18px 22px",boxShadow:accent?`0 0 24px ${C.accent}18`:"none"}}>
//       <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:10}}>
//         <Icon size={15} color={accent?C.accent:C.muted}/>
//         <span style={{fontSize:11,color:C.muted,textTransform:"uppercase" as const,letterSpacing:1}}>{label}</span>
//       </div>
//       <div style={{fontSize:28,fontWeight:700,color:accent?C.accent:C.text,fontVariantNumeric:"tabular-nums"}}>{value}</div>
//       {sub&&<div style={{fontSize:12,color:C.muted,marginTop:4}}>{sub}</div>}
//     </div>
//   );
// }

// function ShardCard({shard,total}:{shard:ShardInfo;total:number}) {
//   const sc = shard.status==="healthy"?C.green:shard.status==="degraded"?C.amber:C.red;
//   const pct = total>0?((shard.vectors/total)*100).toFixed(1):"0.0";
//   return (
//     <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:20,borderTop:`2px solid ${sc}`}}>
//       <div style={{display:"flex",justifyContent:"space-between",alignItems:"center",marginBottom:16}}>
//         <div style={{display:"flex",alignItems:"center",gap:10}}>
//           <div style={{position:"relative",width:28,height:28}}>
//             <div style={{position:"absolute",inset:0,borderRadius:"50%",border:`2px solid ${sc}`,animation:shard.status==="healthy"?"pulse 2.4s ease-in-out infinite":"none"}}/>
//             <div style={{position:"absolute",inset:5,borderRadius:"50%",background:sc}}/>
//           </div>
//           <span style={{fontWeight:600,color:C.text,fontSize:15}}>Shard {shard.id}</span>
//         </div>
//         <span style={{fontSize:10,fontWeight:600,color:sc,background:sc+"22",padding:"2px 8px",borderRadius:20,textTransform:"uppercase" as const,letterSpacing:0.8}}>{shard.status}</span>
//       </div>
//       <div style={{display:"grid",gridTemplateColumns:"1fr 1fr",gap:"12px 20px"}}>
//         <div><div style={{fontSize:10,color:C.muted,marginBottom:2}}>Vectors</div><div style={{fontSize:18,fontWeight:700,color:C.text}}>{fmt(shard.vectors)}</div></div>
//         <div><div style={{fontSize:10,color:C.muted,marginBottom:2}}>Ring Share</div><div style={{fontSize:18,fontWeight:700,color:C.accent}}>{pct}%</div></div>
//       </div>
//       <div style={{marginTop:14,height:4,background:C.border,borderRadius:2}}>
//         <div style={{height:"100%",width:`${pct}%`,borderRadius:2,background:sc,transition:"width 0.5s ease"}}/>
//       </div>
//     </div>
//   );
// }

// type ChartData = {ts:string;p50:number;p95:number;p99:number};
// type ThruData  = {ts:string;insert:number;search:number};
// type VecData   = {ts:string;vectors:number};

// function LatencyChart({history}:{history:MetricsSnapshot[]}) {
//   const data:ChartData[] = history.map(h=>({
//     ts:new Date(h.ts).toLocaleTimeString([],{hour:"2-digit",minute:"2-digit",second:"2-digit"}),
//     p50:+(h.p50Us/1000).toFixed(3), p95:+(h.p95Us/1000).toFixed(3), p99:+(h.p99Us/1000).toFixed(3),
//   }));
//   return (
//     <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"20px 24px"}}>
//       <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:18}}>
//         <Activity size={15} color={C.accent}/>
//         <span style={{fontWeight:600,color:C.text,fontSize:14}}>Search Latency — Live</span>
//         <div style={{marginLeft:"auto",display:"flex",gap:16}}>
//           {([["P50",C.c2],["P95",C.c1],["P99",C.c3]] as [string,string][]).map(([l,c])=>(
//             <div key={l} style={{display:"flex",alignItems:"center",gap:5}}>
//               <div style={{width:8,height:8,borderRadius:2,background:c}}/><span style={{fontSize:11,color:C.muted}}>{l}</span>
//             </div>
//           ))}
//         </div>
//       </div>
//       <ResponsiveContainer width="100%" height={180}>
//         <AreaChart data={data}>
//           <defs>
//             {([["g2",C.c2],["g1",C.c1],["g3",C.c3]] as [string,string][]).map(([id,color])=>(
//               <linearGradient key={id} id={id} x1="0" y1="0" x2="0" y2="1">
//                 <stop offset="5%" stopColor={color} stopOpacity={0.2}/><stop offset="95%" stopColor={color} stopOpacity={0}/>
//               </linearGradient>
//             ))}
//           </defs>
//           <CartesianGrid strokeDasharray="3 3" stroke={C.border}/>
//           <XAxis dataKey="ts" tick={{fill:C.muted,fontSize:9}} tickLine={false} axisLine={false} interval="preserveStartEnd"/>
//           <YAxis tick={{fill:C.muted,fontSize:10}} tickLine={false} axisLine={false} tickFormatter={v=>`${v}ms`}/>
//           <Tooltip contentStyle={{background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,fontSize:12}}
//             formatter={(v:unknown)=>[`${Number(v).toFixed(3)} ms`]}/>
//           <Area type="monotone" dataKey="p50" stroke={C.c2} fill="url(#g2)" strokeWidth={1.5} dot={false}/>
//           <Area type="monotone" dataKey="p95" stroke={C.c1} fill="url(#g1)" strokeWidth={1.5} dot={false}/>
//           <Area type="monotone" dataKey="p99" stroke={C.c3} fill="url(#g3)" strokeWidth={1.5} dot={false}/>
//         </AreaChart>
//       </ResponsiveContainer>
//     </div>
//   );
// }

// function ThroughputChart({history}:{history:MetricsSnapshot[]}) {
//   const data:ThruData[] = history.map(h=>({
//     ts:new Date(h.ts).toLocaleTimeString([],{hour:"2-digit",minute:"2-digit",second:"2-digit"}),
//     insert:h.insertRate, search:h.searchRate,
//   }));
//   return (
//     <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"20px 24px"}}>
//       <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:18}}>
//         <Zap size={15} color={C.green}/>
//         <span style={{fontWeight:600,color:C.text,fontSize:14}}>Throughput — Live</span>
//         <div style={{marginLeft:"auto",display:"flex",gap:16}}>
//           {([["Inserts/s",C.green],["Searches/s",C.accent]] as [string,string][]).map(([l,c])=>(
//             <div key={l} style={{display:"flex",alignItems:"center",gap:5}}>
//               <div style={{width:8,height:8,borderRadius:2,background:c}}/><span style={{fontSize:11,color:C.muted}}>{l}</span>
//             </div>
//           ))}
//         </div>
//       </div>
//       <ResponsiveContainer width="100%" height={180}>
//         <LineChart data={data}>
//           <CartesianGrid strokeDasharray="3 3" stroke={C.border}/>
//           <XAxis dataKey="ts" tick={{fill:C.muted,fontSize:9}} tickLine={false} axisLine={false} interval="preserveStartEnd"/>
//           <YAxis tick={{fill:C.muted,fontSize:10}} tickLine={false} axisLine={false}/>
//           <Tooltip contentStyle={{background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,fontSize:12}}/>
//           <Line type="monotone" dataKey="insert" stroke={C.green} strokeWidth={2} dot={false} name="Inserts/s"/>
//           <Line type="monotone" dataKey="search" stroke={C.accent} strokeWidth={2} dot={false} name="Searches/s"/>
//         </LineChart>
//       </ResponsiveContainer>
//     </div>
//   );
// }

// function VectorGrowthChart({history}:{history:MetricsSnapshot[]}) {
//   const data:VecData[] = history.map(h=>({
//     ts:new Date(h.ts).toLocaleTimeString([],{hour:"2-digit",minute:"2-digit",second:"2-digit"}),
//     vectors:h.totalVectors,
//   }));
//   return (
//     <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"20px 24px"}}>
//       <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:14}}>
//         <Database size={15} color={C.c3}/>
//         <span style={{fontWeight:600,color:C.text,fontSize:14}}>Vector Count Growth</span>
//       </div>
//       <ResponsiveContainer width="100%" height={130}>
//         <AreaChart data={data}>
//           <defs>
//             <linearGradient id="gv" x1="0" y1="0" x2="0" y2="1">
//               <stop offset="5%" stopColor={C.c3} stopOpacity={0.3}/><stop offset="95%" stopColor={C.c3} stopOpacity={0}/>
//             </linearGradient>
//           </defs>
//           <CartesianGrid strokeDasharray="3 3" stroke={C.border}/>
//           <XAxis dataKey="ts" tick={{fill:C.muted,fontSize:9}} tickLine={false} axisLine={false} interval="preserveStartEnd"/>
//           <YAxis tick={{fill:C.muted,fontSize:10}} tickLine={false} axisLine={false} tickFormatter={v=>fmt(v)}/>
//           <Tooltip contentStyle={{background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,fontSize:12}}
//             formatter={(v:unknown)=>[fmt(Number(v)),"vectors"]}/>
//           <Area type="monotone" dataKey="vectors" stroke={C.c3} fill="url(#gv)" strokeWidth={2} dot={false}/>
//         </AreaChart>
//       </ResponsiveContainer>
//     </div>
//   );
// }

// function LoadDistChart({latest}:{latest:MetricsSnapshot|null}) {
//   if (!latest) return null;
//   const data = latest.shards.map(s=>({
//     name:`Shard ${s.id}`,
//     pct: latest.totalVectors>0?+((s.vectors/latest.totalVectors)*100).toFixed(1):0,
//   }));
//   return (
//     <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"20px 24px"}}>
//       <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:14}}>
//         <Layers size={15} color={C.amber}/>
//         <span style={{fontWeight:600,color:C.text,fontSize:14}}>Load Distribution</span>
//       </div>
//       <ResponsiveContainer width="100%" height={130}>
//         <BarChart data={data} barSize={40}>
//           <CartesianGrid strokeDasharray="3 3" stroke={C.border} vertical={false}/>
//           <XAxis dataKey="name" tick={{fill:C.muted,fontSize:11}} tickLine={false} axisLine={false}/>
//           <YAxis tick={{fill:C.muted,fontSize:10}} tickLine={false} axisLine={false} tickFormatter={v=>`${v}%`} domain={[0,100]}/>
//           <Tooltip contentStyle={{background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,fontSize:12}}
//             formatter={(v:unknown)=>[`${Number(v)}%`,"Load"]}/>
//           <Bar dataKey="pct" fill={C.accent} radius={[4,4,0,0]}/>
//         </BarChart>
//       </ResponsiveContainer>
//     </div>
//   );
// }

// // ── Search Playground ─────────────────────────────────────────────────────────

// function SearchPlayground({serverBase,dim}:{serverBase:string;dim:number}) {
//   const [input,setInput]     = useState("");
//   const [topK,setTopK]       = useState(5);
//   const [results,setResults] = useState<SearchResponse|null>(null);
//   const [running,setRunning] = useState(false);
//   const [error,setError]     = useState("");

//   const run = async () => {
//     setError(""); setResults(null); setRunning(true);
//     try {
//       let query:number[];
//       const t = input.trim();
//       if (t) {
//         const p = JSON.parse(t);
//         if (!Array.isArray(p)||p.length!==dim) { setError(`Need a JSON array of ${dim} floats`); setRunning(false); return; }
//         query=p;
//       } else {
//         const raw=Array.from({length:dim},()=>(Math.random()-0.5)*2);
//         const n=Math.sqrt(raw.reduce((s,x)=>s+x*x,0));
//         query=raw.map(x=>x/n);
//       }
//       const resp = await apiSearch(serverBase,query,topK);
//       setResults(resp);
//     } catch(e:unknown) {
//       setError(e instanceof Error?e.message:"Search failed");
//     } finally { setRunning(false); }
//   };

//   return (
//     <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24}}>
//       <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:20}}>
//         <Search size={15} color={C.accent}/>
//         <span style={{fontWeight:600,color:C.text,fontSize:14}}>Search Playground</span>
//         <span style={{fontSize:11,color:C.green,marginLeft:"auto",display:"flex",alignItems:"center",gap:4}}>
//           <span style={{width:6,height:6,borderRadius:"50%",background:C.green,display:"inline-block"}}/> Live — real index
//         </span>
//       </div>

//       <textarea value={input} onChange={e=>setInput(e.target.value)} rows={3}
//         placeholder={`Paste a JSON float array of ${dim} values, or leave blank for a random unit vector`}
//         style={{width:"100%",background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,color:C.text,
//           padding:"10px 14px",fontSize:12,fontFamily:"monospace",resize:"vertical" as const,
//           outline:"none",boxSizing:"border-box" as const}}
//       />

//       {error&&<div style={{marginTop:8,fontSize:12,color:C.red,display:"flex",gap:6,alignItems:"center"}}><AlertTriangle size={13}/>{error}</div>}

//       <div style={{display:"flex",alignItems:"center",gap:12,marginTop:12}}>
//         <label style={{fontSize:12,color:C.muted}}>Top-K</label>
//         <input type="number" min={1} max={100} value={topK} onChange={e=>setTopK(Number(e.target.value))}
//           style={{width:64,background:C.bg,border:`1px solid ${C.border}`,borderRadius:6,color:C.text,padding:"6px 10px",fontSize:12,outline:"none"}}
//         />
//         <button onClick={run} disabled={running} style={{
//           display:"flex",alignItems:"center",gap:6,background:running?C.accentDim:C.accent,
//           color:C.bg,border:"none",borderRadius:8,padding:"8px 18px",fontSize:13,fontWeight:700,
//           cursor:running?"wait":"pointer",marginLeft:"auto",
//         }}>
//           {running?<RefreshCw size={13} style={{animation:"spin 1s linear infinite"}}/>:<Play size={13}/>}
//           {running?"Searching…":"Search"}
//         </button>
//       </div>

//       {results&&(
//         <div style={{marginTop:20}}>
//           <div style={{display:"flex",gap:16,marginBottom:12}}>
//             <span style={{fontSize:11,color:C.muted}}>Latency: <strong style={{color:C.text}}>{fmtUs(results.latency_us)}</strong></span>
//             <span style={{fontSize:11,color:C.muted}}>Shards: <strong style={{color:results.degraded?C.amber:C.green}}>{results.shards_ok}/{results.shards_ok+results.shards_err}</strong></span>
//             {results.degraded&&<span style={{fontSize:11,color:C.amber,display:"flex",alignItems:"center",gap:4}}><AlertTriangle size={11}/> Degraded</span>}
//           </div>
//           {results.results.length===0?(
//             <div style={{textAlign:"center",color:C.muted,padding:"20px 0",fontSize:13}}>
//               Index is empty — insert vectors first (see Insert Demo below).
//             </div>
//           ):(
//             <div style={{display:"flex",flexDirection:"column",gap:6}}>
//               {results.results.map((r,i)=>(
//                 <div key={i} style={{display:"flex",alignItems:"center",gap:12,background:C.bg,borderRadius:8,padding:"10px 14px",border:`1px solid ${C.border}`}}>
//                   <span style={{fontSize:11,color:C.muted,width:24,textAlign:"right"}}>#{i+1}</span>
//                   <div style={{flex:1}}><span style={{fontSize:13,color:C.text}}>node {r.nodeId}</span></div>
//                   <div style={{textAlign:"right"}}>
//                     <div style={{fontSize:13,fontWeight:600,color:C.accent,fontVariantNumeric:"tabular-nums"}}>{r.distance.toFixed(6)}</div>
//                     <div style={{fontSize:10,color:C.muted}}>L2 distance</div>
//                   </div>
//                   <ChevronRight size={14} color={C.muted}/>
//                 </div>
//               ))}
//             </div>
//           )}
//         </div>
//       )}
//     </div>
//   );
// }

// function InsertDemo({dim}:{dim:number}) {
//   const [count,setCount]=useState(200);
//   return (
//     <div style={{background:C.surface,border:`1px solid ${C.amber}33`,borderRadius:12,padding:24,marginTop:16}}>
//       <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:12}}>
//         <Zap size={15} color={C.amber}/>
//         <span style={{fontWeight:600,color:C.text,fontSize:14}}>Insert Test Vectors</span>
//       </div>
//       <div style={{fontSize:12,color:C.muted,marginBottom:14,lineHeight:1.7}}>
//         The browser can't call gRPC directly. Run this Python script in a terminal to populate the index, then search above.
//       </div>
//       <div style={{display:"flex",gap:10,alignItems:"center",marginBottom:14}}>
//         <label style={{fontSize:12,color:C.muted}}>Count</label>
//         <input type="number" min={1} max={50000} value={count} onChange={e=>setCount(Number(e.target.value))}
//           style={{width:90,background:C.bg,border:`1px solid ${C.border}`,borderRadius:6,color:C.text,padding:"6px 10px",fontSize:12,outline:"none"}}
//         />
//       </div>
//       <pre style={{background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,padding:14,fontSize:11,color:C.green,overflow:"auto",fontFamily:"monospace",lineHeight:1.6,whiteSpace:"pre-wrap" as const}}>
// {`# Save as insert_demo.py then run: python3 insert_demo.py
// # Requires: pip install grpcio grpcio-tools protobuf

// import grpc, random, math, struct, sys
// sys.path.insert(0, "./proto")  # path to generated proto stubs

// # If you don't have grpcio, use the HTTP search endpoint and
// # insert vectors manually via a custom gRPC client.

// # Quick alternative — generate and print vectors for manual testing:
// DIM   = ${dim}
// COUNT = ${count}

// def rand_unit(dim):
//     v = [random.gauss(0,1) for _ in range(dim)]
//     n = math.sqrt(sum(x*x for x in v))
//     return [x/n for x in v]

// print(f"Generating {COUNT} random unit vectors (dim={DIM})...")
// for i in range(COUNT):
//     vec = rand_unit(DIM)
//     if i < 3:
//         print(f"  vec[{i}] = {vec[:4]}... (truncated)")

// print(f"Done generating {COUNT} vectors.")
// print("Use your gRPC client to insert these into the vecdb server on port 50051.")`}
//       </pre>
//     </div>
//   );
// }

// // ── Nav ───────────────────────────────────────────────────────────────────────

// type Page = "overview"|"shards"|"search"|"settings";

// function Nav({page,setPage,connected,serverBase,onConnect}:
//   {page:Page;setPage:(p:Page)=>void;connected:boolean;serverBase:string;onConnect:()=>void}) {
//   const tabs:{id:Page;label:string;icon:React.ElementType}[] = [
//     {id:"overview",label:"Overview",icon:Activity},
//     {id:"shards",label:"Shard Map",icon:Server},
//     {id:"search",label:"Search",icon:Search},
//     {id:"settings",label:"Settings",icon:Settings},
//   ];
//   return (
//     <nav style={{background:C.surface,borderBottom:`1px solid ${C.border}`,display:"flex",alignItems:"center",padding:"0 28px",height:56,position:"sticky",top:0,zIndex:100}}>
//       <div style={{display:"flex",alignItems:"center",gap:10,marginRight:32}}>
//         <div style={{width:28,height:28,borderRadius:6,background:`linear-gradient(135deg,${C.accent},${C.c3})`,display:"flex",alignItems:"center",justifyContent:"center"}}>
//           <Database size={14} color="#000"/>
//         </div>
//         <span style={{fontWeight:700,fontSize:15,color:C.text}}>vecdb</span>
//         <span style={{fontSize:10,color:C.muted,background:C.border,padding:"1px 6px",borderRadius:4}}>v0.7</span>
//       </div>
//       <div style={{display:"flex",gap:4}}>
//         {tabs.map(t=>(
//           <button key={t.id} onClick={()=>setPage(t.id)} style={{
//             display:"flex",alignItems:"center",gap:7,
//             background:page===t.id?C.accent+"18":"transparent",
//             border:page===t.id?`1px solid ${C.accent}44`:"1px solid transparent",
//             borderRadius:8,padding:"6px 14px",color:page===t.id?C.accent:C.muted,
//             fontSize:13,fontWeight:page===t.id?600:400,cursor:"pointer",
//           }}><t.icon size={13}/>{t.label}</button>
//         ))}
//       </div>
//       <div style={{marginLeft:"auto",display:"flex",alignItems:"center",gap:10}}>
//         {connected&&<span style={{fontSize:11,color:C.muted,fontFamily:"monospace"}}>{serverBase.replace("http://","")}</span>}
//         <div style={{width:7,height:7,borderRadius:"50%",background:connected?C.green:C.red,boxShadow:connected?`0 0 8px ${C.green}`:"none",animation:connected?"pulse 2s ease-in-out infinite":"none"}}/>
//         <span style={{fontSize:12,color:C.muted}}>{connected?"Live":"Disconnected"}</span>
//         {!connected&&<button onClick={onConnect} style={{background:C.accent,color:C.bg,border:"none",borderRadius:6,padding:"5px 14px",fontSize:12,fontWeight:600,cursor:"pointer"}}>Connect</button>}
//       </div>
//     </nav>
//   );
// }

// // ── Pages ─────────────────────────────────────────────────────────────────────

// function OverviewPage({history,latest,connError}:{history:MetricsSnapshot[];latest:MetricsSnapshot|null;connError:string}) {
//   if (connError) return (
//     <div style={{textAlign:"center",padding:"80px 28px"}}>
//       <AlertTriangle size={32} color={C.red} style={{marginBottom:14}}/>
//       <div style={{fontSize:16,color:C.red,marginBottom:8}}>Connection Error</div>
//       <div style={{fontSize:13,color:C.muted,marginBottom:16}}>{connError}</div>
//       <code style={{background:C.border,padding:"4px 12px",borderRadius:6,fontSize:12,color:C.accent}}>
//         ./build/vecdb_server --no-auth --shards 3 --dim 128
//       </code>
//     </div>
//   );
//   if (!latest) return (
//     <div style={{textAlign:"center",color:C.muted,padding:80}}>
//       <RefreshCw size={24} style={{marginBottom:12,opacity:0.4,animation:"spin 1s linear infinite"}}/>
//       <div>Connecting to server…</div>
//     </div>
//   );
//   const anyDeg = latest.shards.some(s=>s.status!=="healthy");
//   return (
//     <div style={{padding:"28px 28px 48px"}}>
//       {anyDeg&&(
//         <div style={{background:C.amber+"18",border:`1px solid ${C.amber}44`,borderRadius:10,padding:"12px 18px",marginBottom:20,display:"flex",alignItems:"center",gap:10,fontSize:13,color:C.amber}}>
//           <AlertTriangle size={15}/> One or more shards are degraded.
//         </div>
//       )}
//       <div style={{display:"grid",gridTemplateColumns:"repeat(4,1fr)",gap:14,marginBottom:20}}>
//         <StatCard icon={Database} label="Total Vectors"  value={fmt(latest.totalVectors)} sub="real — from HNSW index" accent/>
//         <StatCard icon={Zap}      label="Total Inserts"  value={fmt(latest.totalInserts)}  sub="since server start"/>
//         <StatCard icon={Search}   label="Total Searches" value={fmt(latest.totalSearches)} sub="since server start"/>
//         <StatCard icon={Activity} label="P99 Latency"    value={fmtUs(latest.p99Us)}       sub="last search measured"/>
//       </div>
//       <div style={{display:"grid",gridTemplateColumns:"2fr 1fr",gap:14,marginBottom:14}}>
//         <LatencyChart history={history}/>
//         <div style={{display:"flex",flexDirection:"column",gap:14}}>
//           <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"18px 22px"}}>
//             <div style={{fontSize:11,color:C.muted,textTransform:"uppercase" as const,letterSpacing:1,marginBottom:14}}>Percentiles (live)</div>
//             {([["P50",latest.p50Us,C.c2],["P95",latest.p95Us,C.c1],["P99",latest.p99Us,C.c3]] as [string,number,string][]).map(([l,v,c])=>(
//               <div key={l} style={{marginBottom:12}}>
//                 <div style={{display:"flex",justifyContent:"space-between",marginBottom:4}}>
//                   <span style={{fontSize:12,color:C.muted}}>{l}</span>
//                   <span style={{fontSize:12,fontWeight:600,color:c}}>{fmtUs(v)}</span>
//                 </div>
//                 <div style={{height:3,background:C.border,borderRadius:2}}>
//                   <div style={{height:"100%",borderRadius:2,background:c,width:`${Math.min(100,v>0?(v/2000)*100:0)}%`,transition:"width 0.6s ease"}}/>
//                 </div>
//               </div>
//             ))}
//           </div>
//           <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"18px 22px"}}>
//             <div style={{fontSize:11,color:C.muted,textTransform:"uppercase" as const,letterSpacing:1,marginBottom:14}}>System Health</div>
//             {latest.shards.map(s=>(
//               <div key={s.id} style={{display:"flex",alignItems:"center",gap:10,marginBottom:10}}>
//                 {s.status==="healthy"?<CheckCircle size={14} color={C.green}/>:<AlertTriangle size={14} color={C.amber}/>}
//                 <span style={{fontSize:13,color:C.text}}>Shard {s.id}</span>
//                 <span style={{fontSize:11,color:C.muted,marginLeft:"auto"}}>{fmt(s.vectors)} vecs</span>
//               </div>
//             ))}
//           </div>
//         </div>
//       </div>
//       <div style={{display:"grid",gridTemplateColumns:"1fr 1fr",gap:14}}>
//         <ThroughputChart history={history}/>
//         <div style={{display:"grid",gap:14}}>
//           <VectorGrowthChart history={history}/>
//           <LoadDistChart latest={latest}/>
//         </div>
//       </div>
//     </div>
//   );
// }

// function ShardsPage({latest}:{latest:MetricsSnapshot|null}) {
//   if (!latest) return <div style={{textAlign:"center",color:C.muted,padding:80}}>Waiting for data…</div>;
//   const colors=[C.accent,C.green,C.c3];
//   return (
//     <div style={{padding:"28px 28px 48px"}}>
//       <h2 style={{fontSize:18,fontWeight:700,color:C.text,marginBottom:6}}>Shard Map</h2>
//       <p style={{fontSize:13,color:C.muted,marginBottom:24}}>{latest.shards.length} shards · {fmt(latest.totalVectors)} vectors (real)</p>
//       <div style={{display:"grid",gridTemplateColumns:"repeat(3,1fr)",gap:16,marginBottom:28}}>
//         {latest.shards.map(s=><ShardCard key={s.id} shard={s} total={latest.totalVectors}/>)}
//       </div>
//       <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24}}>
//         <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:20}}>
//           <Cpu size={15} color={C.c3}/><span style={{fontWeight:600,color:C.text,fontSize:14}}>Consistent Hash Ring</span>
//         </div>
//         <svg viewBox="0 0 400 400" style={{width:"100%",maxWidth:380,display:"block",margin:"0 auto"}}>
//           <circle cx="200" cy="200" r="150" fill="none" stroke={C.border} strokeWidth="32"/>
//           {latest.shards.map((s,i)=>{
//             const sa=(i*120-90)*(Math.PI/180), ea=((i+1)*120-90)*(Math.PI/180), r=150;
//             const x1=200+r*Math.cos(sa),y1=200+r*Math.sin(sa),x2=200+r*Math.cos(ea),y2=200+r*Math.sin(ea);
//             const color=colors[i%colors.length];
//             const lp=latest.totalVectors>0?s.vectors/latest.totalVectors:1/latest.shards.length;
//             const la=sa+lp*120*(Math.PI/180);
//             const lx2=200+r*Math.cos(la),ly2=200+r*Math.sin(la);
//             const ma=sa+60*(Math.PI/180);
//             return (
//               <g key={s.id}>
//                 <path d={`M ${x1} ${y1} A ${r} ${r} 0 0 1 ${x2} ${y2}`} fill="none" stroke={color+"30"} strokeWidth="28"/>
//                 <path d={`M ${x1} ${y1} A ${r} ${r} 0 0 1 ${lx2} ${ly2}`} fill="none" stroke={color} strokeWidth="28" opacity={0.85}/>
//                 <text x={200+185*Math.cos(ma)} y={200+185*Math.sin(ma)} fill={color} fontSize="13" fontWeight="700" textAnchor="middle" dominantBaseline="middle">S{s.id}</text>
//               </g>
//             );
//           })}
//           <circle cx="200" cy="200" r="90" fill={C.bg} stroke={C.border} strokeWidth="1"/>
//           <text x="200" y="192" fill={C.text} fontSize="20" fontWeight="700" textAnchor="middle" dominantBaseline="middle">{fmt(latest.totalVectors)}</text>
//           <text x="200" y="215" fill={C.muted} fontSize="11" textAnchor="middle">real vectors</text>
//         </svg>
//         <div style={{display:"flex",justifyContent:"center",gap:24,marginTop:16}}>
//           {latest.shards.map((s,i)=>(
//             <div key={s.id} style={{display:"flex",alignItems:"center",gap:6}}>
//               <div style={{width:10,height:10,borderRadius:2,background:colors[i%colors.length]}}/>
//               <span style={{fontSize:12,color:C.muted}}>S{s.id} — {latest.totalVectors>0?((s.vectors/latest.totalVectors)*100).toFixed(1):0}%</span>
//             </div>
//           ))}
//         </div>
//       </div>
//     </div>
//   );
// }

// function SettingsPage({serverBase,setServerBase}:{serverBase:string;setServerBase:(s:string)=>void}) {
//   const [draft,setDraft]=useState(serverBase);
//   return (
//     <div style={{padding:"28px 28px 48px",maxWidth:600}}>
//       <h2 style={{fontSize:18,fontWeight:700,color:C.text,marginBottom:6}}>Settings</h2>
//       <p style={{fontSize:13,color:C.muted,marginBottom:24}}>Configure the dashboard server connection.</p>
//       <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24}}>
//         <label style={{display:"block",fontSize:12,color:C.muted,marginBottom:8}}>HTTP API Base URL</label>
//         <div style={{display:"flex",gap:10}}>
//           <input value={draft} onChange={e=>setDraft(e.target.value)}
//             style={{flex:1,background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,color:C.text,padding:"10px 14px",fontSize:13,fontFamily:"monospace",outline:"none"}}
//           />
//           <button onClick={()=>setServerBase(draft)} style={{background:C.accent,color:C.bg,border:"none",borderRadius:8,padding:"10px 20px",fontSize:13,fontWeight:700,cursor:"pointer"}}>Apply</button>
//         </div>
//         <p style={{fontSize:11,color:C.muted,marginTop:10}}>Default: http://localhost:8080 (matches --http-port on the server).</p>
//       </div>
//       <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24,marginTop:16}}>
//         <div style={{fontSize:13,fontWeight:600,color:C.text,marginBottom:14}}>API Endpoints</div>
//         {([["GET","/api/health","Health check"],["GET","/api/info","Shard info + vector counts"],["GET","/api/metrics","Latency + throughput (1s rolling)"],["POST","/api/search","ANN search: {query:[...], top_k:N}"]] as [string,string,string][]).map(([method,path,desc])=>(
//           <div key={path} style={{display:"flex",gap:12,marginBottom:10,alignItems:"baseline"}}>
//             <span style={{fontSize:10,fontWeight:700,color:method==="GET"?C.green:C.accent,background:(method==="GET"?C.green:C.accent)+"22",padding:"2px 6px",borderRadius:4,minWidth:40,textAlign:"center" as const}}>{method}</span>
//             <code style={{fontSize:12,color:C.text,fontFamily:"monospace"}}>{path}</code>
//             <span style={{fontSize:12,color:C.muted}}>{desc}</span>
//           </div>
//         ))}
//       </div>
//     </div>
//   );
// }

// // ── App ───────────────────────────────────────────────────────────────────────

// export default function App() {
//   const [page,setPage]           = useState<Page>("overview");
//   const [connected,setConnected] = useState(false);
//   const [serverBase,setServerBase] = useState("http://localhost:8080");
//   const {history,latest,connError} = useLiveMetrics(serverBase,connected);

//   return (
//     <div style={{minHeight:"100vh",background:C.bg,fontFamily:"'Inter','Segoe UI',system-ui,sans-serif",color:C.text}}>
//       <style>{`
//         *{box-sizing:border-box;margin:0;padding:0}
//         ::-webkit-scrollbar{width:6px}
//         ::-webkit-scrollbar-track{background:${C.bg}}
//         ::-webkit-scrollbar-thumb{background:${C.border};border-radius:3px}
//         @keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
//         @keyframes spin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
//         textarea:focus,input:focus{border-color:${C.accent}88!important}
//         button:hover:not(:disabled){opacity:0.88}
//       `}</style>

//       <Nav page={page} setPage={setPage} connected={connected} serverBase={serverBase} onConnect={()=>setConnected(true)}/>

//       {!connected&&(
//         <div style={{background:`linear-gradient(135deg,${C.accent}08,${C.c3}08)`,border:`1px solid ${C.border}`,borderRadius:14,margin:"28px 28px 0",padding:"28px 32px",display:"flex",alignItems:"center",justifyContent:"space-between",gap:24}}>
//           <div>
//             <div style={{fontSize:18,fontWeight:700,color:C.text,marginBottom:8}}>Connect to your vecdb server</div>
//             <div style={{fontSize:13,color:C.muted,lineHeight:1.7}}>
//               Start server:{" "}
//               <code style={{background:C.border,padding:"1px 7px",borderRadius:4,fontSize:12,color:C.accent}}>
//                 ./build/vecdb_server --no-auth --shards 3 --dim 128
//               </code>
//               <br/>
//               Dashboard polls <code style={{color:C.green,fontSize:12}}>{serverBase}/api/metrics</code> every second — all data is real.
//             </div>
//           </div>
//           <button onClick={()=>setConnected(true)} style={{background:C.accent,color:C.bg,border:"none",borderRadius:10,padding:"12px 32px",fontSize:14,fontWeight:700,cursor:"pointer",whiteSpace:"nowrap"}}>
//             Connect
//           </button>
//         </div>
//       )}

//       {page==="overview" &&<OverviewPage history={history} latest={latest} connError={connError}/>}
//       {page==="shards"   &&<ShardsPage   latest={latest}/>}
//       {page==="search"   &&(
//         <div style={{padding:"28px 28px 48px",maxWidth:860}}>
//           <h2 style={{fontSize:18,fontWeight:700,color:C.text,marginBottom:6}}>Search Playground</h2>
//           <p style={{fontSize:13,color:C.muted,marginBottom:20}}>Sends a real POST /api/search to your vecdb server and shows results from the HNSW index.</p>
//           <SearchPlayground serverBase={serverBase} dim={128}/>
//           <InsertDemo dim={128}/>
//         </div>
//       )}
//       {page==="settings" &&<SettingsPage serverBase={serverBase} setServerBase={s=>{setServerBase(s);setConnected(false);}}/>}
//     </div>
//   );
// }



import { useState, useEffect, useRef, useCallback } from "react";
import { AreaChart, Area, LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer, BarChart, Bar } from "recharts";
import { Database, Search, Activity, Server, Zap, AlertTriangle, CheckCircle, RefreshCw, Play, ChevronRight, Layers, Cpu, Settings, Plus, GitBranch } from "lucide-react";

// ── Types ─────────────────────────────────────────────────────────────────────

interface ShardInfo { id: number; vectors: number; status: "healthy"|"degraded"|"offline"; }
interface MetricsSnapshot {
  ts: number; totalVectors: number;
  insertRate: number; searchRate: number;
  p50Us: number; p95Us: number; p99Us: number;
  totalInserts: number; totalSearches: number;
  shards: ShardInfo[];
}
interface SearchResult { nodeId: number; distance: number; }
interface SearchResponse { latency_us: number; degraded: boolean; shards_ok: number; shards_err: number; results: SearchResult[]; }
interface InsertResponse { shard_id: number; node_id: number; vector_id: number; latency_us: number; total_vectors: number; }
interface NodeLayer { layer: number; neighbours: number[]; }
interface NodeResponse { node_id: number; shard_id: number; max_layer: number; total_nodes: number; vector_preview: number[]; layers: NodeLayer[]; }

// ── API ───────────────────────────────────────────────────────────────────────

async function fetchMetrics(base: string): Promise<MetricsSnapshot | null> {
  try {
    const r = await fetch(`${base}/api/metrics`, { signal: AbortSignal.timeout(2000) });
    if (!r.ok) return null;
    const d = await r.json();
    return {
      ts: d.ts, totalVectors: d.total_vectors,
      insertRate: d.insert_rate, searchRate: d.search_rate,
      p50Us: d.p50_us, p95Us: d.p95_us, p99Us: d.p99_us,
      totalInserts: d.total_inserts, totalSearches: d.total_searches,
      shards: (d.shards||[]).map((s:{id:number;vectors:number;status:string}) => ({
        id: s.id, vectors: s.vectors, status: s.status as "healthy"|"degraded"|"offline"
      })),
    };
  } catch { return null; }
}

async function apiSearch(base: string, query: number[], topK: number): Promise<SearchResponse> {
  const r = await fetch(`${base}/api/search`, {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify({query, top_k: topK}), signal: AbortSignal.timeout(5000),
  });
  const d = await r.json();
  if (d.error) throw new Error(d.error);
  return { latency_us:d.latency_us, degraded:d.degraded, shards_ok:d.shards_ok, shards_err:d.shards_err,
    results:(d.results||[]).map((r:{node_id:number;distance:number})=>({nodeId:r.node_id,distance:r.distance})) };
}

async function apiInsert(base: string, vector: number[], vectorId?: number): Promise<InsertResponse> {
  const body: {vector: number[]; vector_id?: number} = { vector };
  if (vectorId !== undefined) body.vector_id = vectorId;
  const r = await fetch(`${base}/api/insert`, {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify(body), signal: AbortSignal.timeout(5000),
  });
  const d = await r.json();
  if (d.error) throw new Error(d.error);
  return d as InsertResponse;
}

async function apiNode(base: string, nodeId: number, shardId: number): Promise<NodeResponse> {
  const r = await fetch(`${base}/api/node?id=${nodeId}&shard=${shardId}`, { signal: AbortSignal.timeout(3000) });
  const d = await r.json();
  if (d.error) throw new Error(d.error);
  return d as NodeResponse;
}

// ── Live metrics hook ─────────────────────────────────────────────────────────

function useLiveMetrics(serverBase: string, connected: boolean) {
  const [history, setHistory] = useState<MetricsSnapshot[]>([]);
  const [latest, setLatest]   = useState<MetricsSnapshot | null>(null);
  const [connError, setConnError] = useState("");
  const ivRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const poll = useCallback(async () => {
    const snap = await fetchMetrics(serverBase);
    if (!snap) { setConnError("Cannot reach server — is it running on port 8080?"); return; }
    setConnError("");
    setLatest(snap);
    setHistory(h => [...h.slice(-59), snap]);
  }, [serverBase]);

  useEffect(() => {
    if (!connected) { if (ivRef.current) clearInterval(ivRef.current); setHistory([]); setLatest(null); return; }
    poll();
    ivRef.current = setInterval(poll, 1000);
    return () => { if (ivRef.current) clearInterval(ivRef.current); };
  }, [connected, poll]);

  return { history, latest, connError };
}

// ── Design tokens ─────────────────────────────────────────────────────────────

const C = {
  bg:"#0A0D12", surface:"#111520", border:"#1E2535",
  accent:"#00D4FF", accentDim:"#0099BB",
  green:"#00E5A0", amber:"#FFB020", red:"#FF4560",
  text:"#E8EDF5", muted:"#5A6478",
  c1:"#00D4FF", c2:"#00E5A0", c3:"#7C5CFF",
};

// ── Helpers ───────────────────────────────────────────────────────────────────

function fmt(n:number,dec=0){
  if(n>=1_000_000)return(n/1_000_000).toFixed(1)+"M";
  if(n>=1_000)return(n/1_000).toFixed(dec)+"K";
  return n.toFixed(dec);
}
function fmtUs(us:number){
  if(us===0)return "—";
  if(us>=1000)return(us/1000).toFixed(1)+" ms";
  return us.toFixed(0)+" µs";
}

// ── Shared UI ─────────────────────────────────────────────────────────────────

function StatCard({icon:Icon,label,value,sub,accent=false}:{icon:React.ElementType;label:string;value:string;sub?:string;accent?:boolean}){
  return(
    <div style={{background:C.surface,border:`1px solid ${accent?C.accent+"44":C.border}`,borderRadius:12,padding:"18px 22px",boxShadow:accent?`0 0 24px ${C.accent}18`:"none"}}>
      <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:10}}>
        <Icon size={15} color={accent?C.accent:C.muted}/>
        <span style={{fontSize:11,color:C.muted,textTransform:"uppercase" as const,letterSpacing:1}}>{label}</span>
      </div>
      <div style={{fontSize:28,fontWeight:700,color:accent?C.accent:C.text,fontVariantNumeric:"tabular-nums"}}>{value}</div>
      {sub&&<div style={{fontSize:12,color:C.muted,marginTop:4}}>{sub}</div>}
    </div>
  );
}

function ShardCard({shard,total}:{shard:ShardInfo;total:number}){
  const sc=shard.status==="healthy"?C.green:shard.status==="degraded"?C.amber:C.red;
  const pct=total>0?((shard.vectors/total)*100).toFixed(1):"0.0";
  return(
    <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:20,borderTop:`2px solid ${sc}`}}>
      <div style={{display:"flex",justifyContent:"space-between",alignItems:"center",marginBottom:16}}>
        <div style={{display:"flex",alignItems:"center",gap:10}}>
          <div style={{position:"relative",width:28,height:28}}>
            <div style={{position:"absolute",inset:0,borderRadius:"50%",border:`2px solid ${sc}`,animation:shard.status==="healthy"?"pulse 2.4s ease-in-out infinite":"none"}}/>
            <div style={{position:"absolute",inset:5,borderRadius:"50%",background:sc}}/>
          </div>
          <span style={{fontWeight:600,color:C.text,fontSize:15}}>Shard {shard.id}</span>
        </div>
        <span style={{fontSize:10,fontWeight:600,color:sc,background:sc+"22",padding:"2px 8px",borderRadius:20,textTransform:"uppercase" as const}}>{shard.status}</span>
      </div>
      <div style={{display:"grid",gridTemplateColumns:"1fr 1fr",gap:"12px 20px"}}>
        <div><div style={{fontSize:10,color:C.muted,marginBottom:2}}>Vectors</div><div style={{fontSize:18,fontWeight:700,color:C.text}}>{fmt(shard.vectors)}</div></div>
        <div><div style={{fontSize:10,color:C.muted,marginBottom:2}}>Ring Share</div><div style={{fontSize:18,fontWeight:700,color:C.accent}}>{pct}%</div></div>
      </div>
      <div style={{marginTop:14,height:4,background:C.border,borderRadius:2}}>
        <div style={{height:"100%",width:`${pct}%`,borderRadius:2,background:sc,transition:"width 0.5s ease"}}/>
      </div>
    </div>
  );
}

// ── Charts ────────────────────────────────────────────────────────────────────

function LatencyChart({history}:{history:MetricsSnapshot[]}){
  const data=history.map(h=>({
    ts:new Date(h.ts).toLocaleTimeString([],{hour:"2-digit",minute:"2-digit",second:"2-digit"}),
    p50:+(h.p50Us/1000).toFixed(3),p95:+(h.p95Us/1000).toFixed(3),p99:+(h.p99Us/1000).toFixed(3),
  }));
  return(
    <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"20px 24px"}}>
      <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:18}}>
        <Activity size={15} color={C.accent}/>
        <span style={{fontWeight:600,color:C.text,fontSize:14}}>Search Latency — Live</span>
        <div style={{marginLeft:"auto",display:"flex",gap:16}}>
          {([["P50",C.c2],["P95",C.c1],["P99",C.c3]] as [string,string][]).map(([l,c])=>(
            <div key={l} style={{display:"flex",alignItems:"center",gap:5}}>
              <div style={{width:8,height:8,borderRadius:2,background:c}}/><span style={{fontSize:11,color:C.muted}}>{l}</span>
            </div>
          ))}
        </div>
      </div>
      <ResponsiveContainer width="100%" height={180}>
        <AreaChart data={data}>
          <defs>
            {([["g2",C.c2],["g1",C.c1],["g3",C.c3]] as [string,string][]).map(([id,color])=>(
              <linearGradient key={id} id={id} x1="0" y1="0" x2="0" y2="1">
                <stop offset="5%" stopColor={color} stopOpacity={0.2}/><stop offset="95%" stopColor={color} stopOpacity={0}/>
              </linearGradient>
            ))}
          </defs>
          <CartesianGrid strokeDasharray="3 3" stroke={C.border}/>
          <XAxis dataKey="ts" tick={{fill:C.muted,fontSize:9}} tickLine={false} axisLine={false} interval="preserveStartEnd"/>
          <YAxis tick={{fill:C.muted,fontSize:10}} tickLine={false} axisLine={false} tickFormatter={v=>`${v}ms`}/>
          <Tooltip contentStyle={{background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,fontSize:12}} formatter={(v:unknown)=>[`${Number(v).toFixed(3)} ms`]}/>
          <Area type="monotone" dataKey="p50" stroke={C.c2} fill="url(#g2)" strokeWidth={1.5} dot={false}/>
          <Area type="monotone" dataKey="p95" stroke={C.c1} fill="url(#g1)" strokeWidth={1.5} dot={false}/>
          <Area type="monotone" dataKey="p99" stroke={C.c3} fill="url(#g3)" strokeWidth={1.5} dot={false}/>
        </AreaChart>
      </ResponsiveContainer>
    </div>
  );
}

function ThroughputChart({history}:{history:MetricsSnapshot[]}){
  const data=history.map(h=>({
    ts:new Date(h.ts).toLocaleTimeString([],{hour:"2-digit",minute:"2-digit",second:"2-digit"}),
    insert:h.insertRate, search:h.searchRate,
  }));
  return(
    <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"20px 24px"}}>
      <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:18}}>
        <Zap size={15} color={C.green}/>
        <span style={{fontWeight:600,color:C.text,fontSize:14}}>Throughput — Live</span>
        <div style={{marginLeft:"auto",display:"flex",gap:16}}>
          {([["Inserts/s",C.green],["Searches/s",C.accent]] as [string,string][]).map(([l,c])=>(
            <div key={l} style={{display:"flex",alignItems:"center",gap:5}}>
              <div style={{width:8,height:8,borderRadius:2,background:c}}/><span style={{fontSize:11,color:C.muted}}>{l}</span>
            </div>
          ))}
        </div>
      </div>
      <ResponsiveContainer width="100%" height={180}>
        <LineChart data={data}>
          <CartesianGrid strokeDasharray="3 3" stroke={C.border}/>
          <XAxis dataKey="ts" tick={{fill:C.muted,fontSize:9}} tickLine={false} axisLine={false} interval="preserveStartEnd"/>
          <YAxis tick={{fill:C.muted,fontSize:10}} tickLine={false} axisLine={false}/>
          <Tooltip contentStyle={{background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,fontSize:12}}/>
          <Line type="monotone" dataKey="insert" stroke={C.green} strokeWidth={2} dot={false} name="Inserts/s"/>
          <Line type="monotone" dataKey="search" stroke={C.accent} strokeWidth={2} dot={false} name="Searches/s"/>
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}

function VectorGrowthChart({history}:{history:MetricsSnapshot[]}){
  const data=history.map(h=>({
    ts:new Date(h.ts).toLocaleTimeString([],{hour:"2-digit",minute:"2-digit",second:"2-digit"}),
    vectors:h.totalVectors,
  }));
  return(
    <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"20px 24px"}}>
      <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:14}}>
        <Database size={15} color={C.c3}/>
        <span style={{fontWeight:600,color:C.text,fontSize:14}}>Vector Count Growth</span>
      </div>
      <ResponsiveContainer width="100%" height={130}>
        <AreaChart data={data}>
          <defs>
            <linearGradient id="gv" x1="0" y1="0" x2="0" y2="1">
              <stop offset="5%" stopColor={C.c3} stopOpacity={0.3}/><stop offset="95%" stopColor={C.c3} stopOpacity={0}/>
            </linearGradient>
          </defs>
          <CartesianGrid strokeDasharray="3 3" stroke={C.border}/>
          <XAxis dataKey="ts" tick={{fill:C.muted,fontSize:9}} tickLine={false} axisLine={false} interval="preserveStartEnd"/>
          <YAxis tick={{fill:C.muted,fontSize:10}} tickLine={false} axisLine={false} tickFormatter={v=>fmt(v)}/>
          <Tooltip contentStyle={{background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,fontSize:12}} formatter={(v:unknown)=>[fmt(Number(v)),"vectors"]}/>
          <Area type="monotone" dataKey="vectors" stroke={C.c3} fill="url(#gv)" strokeWidth={2} dot={false}/>
        </AreaChart>
      </ResponsiveContainer>
    </div>
  );
}

function LoadDistChart({latest}:{latest:MetricsSnapshot|null}){
  if(!latest)return null;
  const data=latest.shards.map(s=>({name:`Shard ${s.id}`,pct:latest.totalVectors>0?+((s.vectors/latest.totalVectors)*100).toFixed(1):0}));
  return(
    <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"20px 24px"}}>
      <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:14}}>
        <Layers size={15} color={C.amber}/><span style={{fontWeight:600,color:C.text,fontSize:14}}>Load Distribution</span>
      </div>
      <ResponsiveContainer width="100%" height={130}>
        <BarChart data={data} barSize={40}>
          <CartesianGrid strokeDasharray="3 3" stroke={C.border} vertical={false}/>
          <XAxis dataKey="name" tick={{fill:C.muted,fontSize:11}} tickLine={false} axisLine={false}/>
          <YAxis tick={{fill:C.muted,fontSize:10}} tickLine={false} axisLine={false} tickFormatter={v=>`${v}%`} domain={[0,100]}/>
          <Tooltip contentStyle={{background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,fontSize:12}} formatter={(v:unknown)=>[`${Number(v)}%`,"Load"]}/>
          <Bar dataKey="pct" fill={C.accent} radius={[4,4,0,0]}/>
        </BarChart>
      </ResponsiveContainer>
    </div>
  );
}

// ── INSERT PAGE ───────────────────────────────────────────────────────────────

function InsertPage({serverBase,dim}:{serverBase:string;dim:number}){
  const [input,setInput]         = useState("");
  const [vectorId,setVectorId]   = useState("");
  const [result,setResult]       = useState<InsertResponse|null>(null);
  const [error,setError]         = useState("");
  const [running,setRunning]     = useState(false);
  const [bulkCount,setBulkCount] = useState(100);
  const [bulkRunning,setBulkRunning] = useState(false);
  const [bulkProgress,setBulkProgress] = useState<{done:number;total:number}|null>(null);
  const [bulkDone,setBulkDone]   = useState(false);
  const stopBulk = useRef(false);

  const randUnitVec = (d:number) => {
    const v = Array.from({length:d},()=>(Math.random()-0.5)*2);
    const n = Math.sqrt(v.reduce((s,x)=>s+x*x,0));
    return v.map(x=>x/n);
  };

  const doInsert = async () => {
    setError(""); setResult(null); setRunning(true);
    try {
      let vec: number[];
      const t = input.trim();
      if (t) {
        const p = JSON.parse(t);
        if (!Array.isArray(p)||p.length!==dim){setError(`Need a JSON array of ${dim} floats`);setRunning(false);return;}
        vec=p;
      } else {
        vec = randUnitVec(dim);
      }
      const vid = vectorId.trim() ? parseInt(vectorId) : undefined;
      const resp = await apiInsert(serverBase, vec, vid);
      setResult(resp);
    } catch(e:unknown){
      setError(e instanceof Error?e.message:"Insert failed");
    } finally { setRunning(false); }
  };

  const doBulk = async () => {
    setBulkRunning(true); setBulkDone(false); setBulkProgress(null);
    stopBulk.current = false;
    const total = bulkCount;
    for (let i=0;i<total;i++){
      if (stopBulk.current) break;
      const vec = randUnitVec(dim);
      try { await apiInsert(serverBase, vec); }
      catch(e) { console.error("Insert failed:", e); }
      if (i%10===0) setBulkProgress({done:i+1,total});
    }
    setBulkProgress({done:total,total});
    setBulkRunning(false);
    setBulkDone(true);
  };

  const btn = (label:string,onClick:()=>void,disabled:boolean,color=C.accent) => (
    <button onClick={onClick} disabled={disabled} style={{
      display:"flex",alignItems:"center",gap:6,background:disabled?C.muted:color,
      color:C.bg,border:"none",borderRadius:8,padding:"8px 18px",fontSize:13,
      fontWeight:700,cursor:disabled?"not-allowed":"pointer",transition:"all 0.2s",
    }}>{label}</button>
  );

  return(
    <div style={{padding:"28px 28px 48px",maxWidth:860}}>
      <div style={{marginBottom:24}}>
        <h2 style={{fontSize:18,fontWeight:700,color:C.text,marginBottom:6}}>Insert Vectors</h2>
        <p style={{fontSize:13,color:C.muted}}>
          Insert vectors directly into the HNSW graph via the browser.
          Each insert goes through the consistent hash ring → WAL → HNSW index.
        </p>
      </div>

      {/* ── Single insert ── */}
      <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24,marginBottom:16}}>
        <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:20}}>
          <Plus size={15} color={C.green}/>
          <span style={{fontWeight:600,color:C.text,fontSize:14}}>Single Insert</span>
          <span style={{fontSize:11,color:C.muted,marginLeft:8}}>Leave blank for a random unit vector</span>
        </div>

        <textarea value={input} onChange={e=>setInput(e.target.value)} rows={3}
          placeholder={`Paste a JSON float array of ${dim} values, e.g. [0.12, -0.34, 0.56, ...]`}
          style={{width:"100%",background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,
            color:C.text,padding:"10px 14px",fontSize:12,fontFamily:"monospace",
            resize:"vertical" as const,outline:"none",boxSizing:"border-box" as const}}
        />

        <div style={{display:"flex",alignItems:"center",gap:12,marginTop:12}}>
          <label style={{fontSize:12,color:C.muted,whiteSpace:"nowrap" as const}}>Vector ID (optional)</label>
          <input type="number" value={vectorId} onChange={e=>setVectorId(e.target.value)}
            placeholder="auto"
            style={{width:100,background:C.bg,border:`1px solid ${C.border}`,borderRadius:6,
              color:C.text,padding:"6px 10px",fontSize:12,outline:"none"}}
          />
          <div style={{marginLeft:"auto"}}>
            {btn(running?"Inserting…":"Insert Vector",doInsert,running,C.green)}
          </div>
        </div>

        {error&&<div style={{marginTop:10,fontSize:12,color:C.red,display:"flex",gap:6,alignItems:"center"}}><AlertTriangle size={13}/>{error}</div>}

        {result&&(
          <div style={{marginTop:16,background:C.bg,borderRadius:8,padding:14,border:`1px solid ${C.green}44`}}>
            <div style={{fontSize:11,color:C.green,fontWeight:600,marginBottom:10,textTransform:"uppercase" as const,letterSpacing:0.8}}>
              ✓ Inserted successfully
            </div>
            <div style={{display:"grid",gridTemplateColumns:"repeat(4,1fr)",gap:12}}>
              {[["Vector ID",result.vector_id],["Shard",result.shard_id],["Node ID",result.node_id],["Latency",fmtUs(result.latency_us)]].map(([k,v])=>(
                <div key={String(k)}>
                  <div style={{fontSize:10,color:C.muted,marginBottom:3}}>{k}</div>
                  <div style={{fontSize:14,fontWeight:600,color:C.text,fontVariantNumeric:"tabular-nums"}}>{String(v)}</div>
                </div>
              ))}
            </div>
            <div style={{marginTop:10,fontSize:12,color:C.muted}}>
              Total vectors in index: <strong style={{color:C.accent}}>{fmt(result.total_vectors)}</strong>
            </div>
          </div>
        )}
      </div>

      {/* ── Bulk insert ── */}
      <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24}}>
        <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:16}}>
          <Zap size={15} color={C.amber}/>
          <span style={{fontWeight:600,color:C.text,fontSize:14}}>Bulk Insert — Random Vectors</span>
          <span style={{fontSize:11,color:C.muted,marginLeft:8}}>Populates the index with random unit vectors for testing</span>
        </div>

        <div style={{fontSize:12,color:C.muted,marginBottom:14,lineHeight:1.7}}>
          Inserts N random unit vectors (dim={dim}) one by one via <code style={{color:C.accent,fontSize:11}}>POST /api/insert</code>.
          Watch the Overview dashboard — Total Vectors will increment in real time.
        </div>

        <div style={{display:"flex",alignItems:"center",gap:12,flexWrap:"wrap" as const}}>
          <label style={{fontSize:12,color:C.muted}}>Count</label>
          <input type="number" min={1} max={50000} value={bulkCount} onChange={e=>setBulkCount(Number(e.target.value))}
            style={{width:90,background:C.bg,border:`1px solid ${C.border}`,borderRadius:6,
              color:C.text,padding:"6px 10px",fontSize:12,outline:"none"}}
          />
          {!bulkRunning
            ? btn(`Insert ${bulkCount} Vectors`, doBulk, false, C.amber)
            : <button onClick={()=>{stopBulk.current=true;}} style={{
                background:C.red,color:"#fff",border:"none",borderRadius:8,
                padding:"8px 16px",fontSize:13,fontWeight:700,cursor:"pointer"}}>Stop</button>
          }
        </div>

        {bulkProgress&&(
          <div style={{marginTop:14}}>
            <div style={{display:"flex",justifyContent:"space-between",marginBottom:6}}>
              <span style={{fontSize:12,color:C.muted}}>
                {bulkDone?"Complete":"Inserting…"} {bulkProgress.done}/{bulkProgress.total}
              </span>
              <span style={{fontSize:12,color:C.accent}}>{Math.round((bulkProgress.done/bulkProgress.total)*100)}%</span>
            </div>
            <div style={{height:6,background:C.border,borderRadius:3,overflow:"hidden"}}>
              <div style={{
                height:"100%",borderRadius:3,
                background:bulkDone?C.green:C.amber,
                width:`${(bulkProgress.done/bulkProgress.total)*100}%`,
                transition:"width 0.2s ease",
              }}/>
            </div>
            {bulkDone&&(
              <div style={{marginTop:10,fontSize:12,color:C.green}}>
                ✓ Inserted {bulkProgress.total} vectors. Go to Overview or Search to see them.
              </div>
            )}
          </div>
        )}
      </div>
    </div>
  );
}

// ── GRAPH INSPECTOR PAGE ──────────────────────────────────────────────────────

function GraphInspectorPage({serverBase,latest}:{serverBase:string;latest:MetricsSnapshot|null}){
  const [nodeId,setNodeId]   = useState("0");
  const [shardId,setShardId] = useState("0");
  const [data,setData]       = useState<NodeResponse|null>(null);
  const [error,setError]     = useState("");
  const [loading,setLoading] = useState(false);

  const layerColors = [C.accent, C.green, C.c3, C.amber];

  const inspect = async () => {
    setError(""); setData(null); setLoading(true);
    try {
      const resp = await apiNode(serverBase, parseInt(nodeId)||0, parseInt(shardId)||0);
      setData(resp);
    } catch(e:unknown){
      setError(e instanceof Error?e.message:"Failed to fetch node");
    } finally { setLoading(false); }
  };

  // SVG force-layout: arrange neighbours in a circle around the centre node
  const renderGraph = (node: NodeResponse) => {
    const W=500, H=340, cx=250, cy=170, R=120;
    // Only show layer 0 neighbours (densest, most interesting)
    const layer0 = node.layers.find(l=>l.layer===0);
    const nbrs   = layer0?.neighbours.slice(0,12) ?? []; // cap at 12 for clarity

    const angle = (i:number) => (i/nbrs.length)*2*Math.PI - Math.PI/2;
    const nx = (i:number) => cx + R*Math.cos(angle(i));
    const ny = (i:number) => cy + R*Math.sin(angle(i));

    return(
      <svg viewBox={`0 0 ${W} ${H}`} style={{width:"100%",maxWidth:500,display:"block",margin:"0 auto"}}>
        {/* Edges */}
        {nbrs.map((_,i)=>(
          <line key={i} x1={cx} y1={cy} x2={nx(i)} y2={ny(i)}
            stroke={C.accent} strokeWidth={1} strokeOpacity={0.4}/>
        ))}
        {/* Neighbour nodes */}
        {nbrs.map((nbr,i)=>(
          <g key={i}>
            <circle cx={nx(i)} cy={ny(i)} r={16} fill={C.surface} stroke={C.c2} strokeWidth={1.5}/>
            <text x={nx(i)} y={ny(i)} fill={C.c2} fontSize={9} textAnchor="middle" dominantBaseline="middle"
              fontWeight="600">{nbr}</text>
          </g>
        ))}
        {/* Centre node */}
        <circle cx={cx} cy={cy} r={22} fill={C.accent} opacity={0.9}/>
        <text x={cx} y={cy-4} fill={C.bg} fontSize={11} textAnchor="middle" dominantBaseline="middle" fontWeight="700">
          {node.node_id}
        </text>
        <text x={cx} y={cy+10} fill={C.bg} fontSize={8} textAnchor="middle">node</text>

        {/* Legend */}
        <circle cx={20} cy={H-20} r={8} fill={C.accent} opacity={0.9}/>
        <text x={34} y={H-16} fill={C.muted} fontSize={9}>query node</text>
        <circle cx={110} cy={H-20} r={8} fill={C.surface} stroke={C.c2} strokeWidth={1.5}/>
        <text x={124} y={H-16} fill={C.muted} fontSize={9}>L0 neighbour</text>
      </svg>
    );
  };

  const totalVectors = latest?.shards[parseInt(shardId)||0]?.vectors ?? 0;

  return(
    <div style={{padding:"28px 28px 48px",maxWidth:900}}>
      <div style={{marginBottom:24}}>
        <h2 style={{fontSize:18,fontWeight:700,color:C.text,marginBottom:6}}>HNSW Graph Inspector</h2>
        <p style={{fontSize:13,color:C.muted}}>
          Inspect any node's connections in the HNSW proximity graph.
          Shows neighbours at each layer — Layer 0 is the dense base graph, higher layers are sparse highway links.
        </p>
      </div>

      {/* Controls */}
      <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24,marginBottom:16}}>
        <div style={{display:"flex",alignItems:"center",gap:12,flexWrap:"wrap" as const}}>
          <div>
            <label style={{display:"block",fontSize:11,color:C.muted,marginBottom:4,textTransform:"uppercase" as const,letterSpacing:0.8}}>Node ID</label>
            <input type="number" min={0} value={nodeId} onChange={e=>setNodeId(e.target.value)}
              style={{width:100,background:C.bg,border:`1px solid ${C.border}`,borderRadius:6,
                color:C.text,padding:"8px 12px",fontSize:13,outline:"none"}}
            />
            {totalVectors>0&&<div style={{fontSize:10,color:C.muted,marginTop:4}}>0 – {totalVectors-1}</div>}
          </div>
          <div>
            <label style={{display:"block",fontSize:11,color:C.muted,marginBottom:4,textTransform:"uppercase" as const,letterSpacing:0.8}}>Shard</label>
            <select value={shardId} onChange={e=>setShardId(e.target.value)}
              style={{background:C.bg,border:`1px solid ${C.border}`,borderRadius:6,
                color:C.text,padding:"8px 12px",fontSize:13,outline:"none"}}>
              {(latest?.shards??[{id:0},{id:1},{id:2}]).map(s=>(
                <option key={s.id} value={s.id}>Shard {s.id}</option>
              ))}
            </select>
          </div>
          <div style={{alignSelf:"flex-end"}}>
            <button onClick={inspect} disabled={loading} style={{
              display:"flex",alignItems:"center",gap:6,
              background:loading?C.muted:C.c3,color:"#fff",border:"none",
              borderRadius:8,padding:"9px 20px",fontSize:13,fontWeight:700,cursor:loading?"wait":"pointer",
            }}>
              {loading?<RefreshCw size={13} style={{animation:"spin 1s linear infinite"}}/>:<GitBranch size={13}/>}
              {loading?"Loading…":"Inspect Node"}
            </button>
          </div>
          {data&&(
            <div style={{marginLeft:"auto",display:"flex",gap:16}}>
              <div><div style={{fontSize:10,color:C.muted}}>Max Layer</div><div style={{fontSize:16,fontWeight:700,color:C.accent}}>{data.max_layer}</div></div>
              <div><div style={{fontSize:10,color:C.muted}}>Layer 0 Nbrs</div><div style={{fontSize:16,fontWeight:700,color:C.c2}}>{data.layers[0]?.neighbours.length??0}</div></div>
              <div><div style={{fontSize:10,color:C.muted}}>Total Nodes</div><div style={{fontSize:16,fontWeight:700,color:C.text}}>{fmt(data.total_nodes)}</div></div>
            </div>
          )}
        </div>
        {error&&<div style={{marginTop:10,fontSize:12,color:C.red,display:"flex",gap:6,alignItems:"center"}}><AlertTriangle size={13}/>{error}</div>}
      </div>

      {data&&(
        <div style={{display:"grid",gridTemplateColumns:"1fr 1fr",gap:16}}>
          {/* SVG graph */}
          <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24}}>
            <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:16}}>
              <GitBranch size={14} color={C.accent}/>
              <span style={{fontWeight:600,color:C.text,fontSize:13}}>Layer 0 — Local Neighbourhood</span>
            </div>
            {renderGraph(data)}
            <div style={{fontSize:11,color:C.muted,textAlign:"center" as const,marginTop:8}}>
              Showing up to 12 of {data.layers[0]?.neighbours.length??0} neighbours
            </div>
          </div>

          {/* Layer breakdown */}
          <div style={{display:"flex",flexDirection:"column" as const,gap:12}}>
            {/* Vector preview */}
            <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:20}}>
              <div style={{fontSize:11,color:C.muted,marginBottom:10,textTransform:"uppercase" as const,letterSpacing:0.8}}>
                Vector Preview (first {data.vector_preview.length} dims)
              </div>
              <div style={{fontFamily:"monospace",fontSize:11,color:C.green,lineHeight:1.8,wordBreak:"break-all" as const}}>
                [{data.vector_preview.map(f=>f.toFixed(4)).join(", ")}…]
              </div>
            </div>

            {/* Per-layer neighbour counts */}
            <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:20}}>
              <div style={{fontSize:11,color:C.muted,marginBottom:14,textTransform:"uppercase" as const,letterSpacing:0.8}}>
                Connections per Layer
              </div>
              {data.layers.map((l,i)=>(
                <div key={l.layer} style={{marginBottom:14}}>
                  <div style={{display:"flex",justifyContent:"space-between",marginBottom:5}}>
                    <span style={{fontSize:12,color:layerColors[i%4],fontWeight:600}}>
                      Layer {l.layer}{l.layer===0?" (dense base)":l.layer===data.max_layer?" (top highway)":""}
                    </span>
                    <span style={{fontSize:12,color:C.text,fontWeight:600}}>{l.neighbours.length} links</span>
                  </div>
                  <div style={{height:4,background:C.border,borderRadius:2}}>
                    <div style={{height:"100%",borderRadius:2,background:layerColors[i%4],
                      width:`${Math.min(100,(l.neighbours.length/32)*100)}%`}}/>
                  </div>
                  <div style={{marginTop:4,fontSize:10,color:C.muted,fontFamily:"monospace",
                    whiteSpace:"nowrap" as const,overflow:"hidden",textOverflow:"ellipsis"}}>
                    {l.neighbours.slice(0,8).join(", ")}{l.neighbours.length>8?`, +${l.neighbours.length-8} more`:""}
                  </div>
                </div>
              ))}
            </div>
          </div>
        </div>
      )}

      {!data&&!error&&(
        <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:40,textAlign:"center" as const}}>
          <GitBranch size={32} color={C.muted} style={{marginBottom:14,opacity:0.4}}/>
          <div style={{fontSize:14,color:C.muted,marginBottom:8}}>Enter a node ID and click Inspect</div>
          <div style={{fontSize:12,color:C.muted}}>
            Insert some vectors first, then inspect node 0 to see its HNSW graph connections.
          </div>
        </div>
      )}
    </div>
  );
}

// ── SEARCH PAGE ───────────────────────────────────────────────────────────────

function SearchPage({serverBase,dim}:{serverBase:string;dim:number}){
  const [input,setInput]     = useState("");
  const [topK,setTopK]       = useState(5);
  const [results,setResults] = useState<SearchResponse|null>(null);
  const [running,setRunning] = useState(false);
  const [error,setError]     = useState("");

  const run = async () => {
    setError(""); setResults(null); setRunning(true);
    try {
      let query:number[];
      const t=input.trim();
      if(t){
        const p=JSON.parse(t);
        if(!Array.isArray(p)||p.length!==dim){setError(`Need a JSON array of ${dim} floats`);setRunning(false);return;}
        query=p;
      } else {
        const raw=Array.from({length:dim},()=>(Math.random()-0.5)*2);
        const n=Math.sqrt(raw.reduce((s,x)=>s+x*x,0));
        query=raw.map(x=>x/n);
      }
      const resp=await apiSearch(serverBase,query,topK);
      setResults(resp);
    } catch(e:unknown){
      setError(e instanceof Error?e.message:"Search failed");
    } finally{setRunning(false);}
  };

  return(
    <div style={{padding:"28px 28px 48px",maxWidth:860}}>
      <div style={{marginBottom:20}}>
        <h2 style={{fontSize:18,fontWeight:700,color:C.text,marginBottom:6}}>Search Playground</h2>
        <p style={{fontSize:13,color:C.muted}}>
          Runs a real ANN search across all shards via scatter-gather. Leave blank for a random unit vector.
        </p>
      </div>
      <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24}}>
        <textarea value={input} onChange={e=>setInput(e.target.value)} rows={3}
          placeholder={`Paste a JSON float array of ${dim} values, or leave blank for random`}
          style={{width:"100%",background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,
            color:C.text,padding:"10px 14px",fontSize:12,fontFamily:"monospace",
            resize:"vertical" as const,outline:"none",boxSizing:"border-box" as const}}
        />
        {error&&<div style={{marginTop:8,fontSize:12,color:C.red,display:"flex",gap:6,alignItems:"center"}}><AlertTriangle size={13}/>{error}</div>}
        <div style={{display:"flex",alignItems:"center",gap:12,marginTop:12}}>
          <label style={{fontSize:12,color:C.muted}}>Top-K</label>
          <input type="number" min={1} max={100} value={topK} onChange={e=>setTopK(Number(e.target.value))}
            style={{width:64,background:C.bg,border:`1px solid ${C.border}`,borderRadius:6,color:C.text,padding:"6px 10px",fontSize:12,outline:"none"}}
          />
          <button onClick={run} disabled={running} style={{
            display:"flex",alignItems:"center",gap:6,background:running?C.accentDim:C.accent,
            color:C.bg,border:"none",borderRadius:8,padding:"8px 18px",fontSize:13,fontWeight:700,
            cursor:running?"wait":"pointer",marginLeft:"auto",
          }}>
            {running?<RefreshCw size={13} style={{animation:"spin 1s linear infinite"}}/>:<Play size={13}/>}
            {running?"Searching…":"Search"}
          </button>
        </div>
        {results&&(
          <div style={{marginTop:20}}>
            <div style={{display:"flex",gap:16,marginBottom:12}}>
              <span style={{fontSize:11,color:C.muted}}>Latency: <strong style={{color:C.text}}>{fmtUs(results.latency_us)}</strong></span>
              <span style={{fontSize:11,color:C.muted}}>Shards: <strong style={{color:results.degraded?C.amber:C.green}}>{results.shards_ok}/{results.shards_ok+results.shards_err}</strong></span>
              {results.degraded&&<span style={{fontSize:11,color:C.amber,display:"flex",alignItems:"center",gap:4}}><AlertTriangle size={11}/> Degraded</span>}
            </div>
            {results.results.length===0
              ?<div style={{textAlign:"center" as const,color:C.muted,padding:"20px 0",fontSize:13}}>Index is empty — go to Insert page to add vectors first.</div>
              :<div style={{display:"flex",flexDirection:"column" as const,gap:6}}>
                {results.results.map((r,i)=>(
                  <div key={i} style={{display:"flex",alignItems:"center",gap:12,background:C.bg,borderRadius:8,padding:"10px 14px",border:`1px solid ${C.border}`}}>
                    <span style={{fontSize:11,color:C.muted,width:24,textAlign:"right" as const}}>#{i+1}</span>
                    <div style={{flex:1}}><span style={{fontSize:13,color:C.text}}>node {r.nodeId}</span></div>
                    <div style={{textAlign:"right" as const}}>
                      <div style={{fontSize:13,fontWeight:600,color:C.accent,fontVariantNumeric:"tabular-nums"}}>{r.distance.toFixed(6)}</div>
                      <div style={{fontSize:10,color:C.muted}}>L2 distance</div>
                    </div>
                    <ChevronRight size={14} color={C.muted}/>
                  </div>
                ))}
              </div>
            }
          </div>
        )}
      </div>
    </div>
  );
}

// ── OVERVIEW PAGE ─────────────────────────────────────────────────────────────

function OverviewPage({history,latest,connError}:{history:MetricsSnapshot[];latest:MetricsSnapshot|null;connError:string}){
  if(connError)return(
    <div style={{textAlign:"center" as const,padding:"80px 28px"}}>
      <AlertTriangle size={32} color={C.red} style={{marginBottom:14}}/>
      <div style={{fontSize:16,color:C.red,marginBottom:8}}>Connection Error</div>
      <div style={{fontSize:13,color:C.muted,marginBottom:16}}>{connError}</div>
      <code style={{background:C.border,padding:"4px 12px",borderRadius:6,fontSize:12,color:C.accent}}>
        ./build/vecdb_server --no-auth --shards 3 --dim 128
      </code>
    </div>
  );
  if(!latest)return(
    <div style={{textAlign:"center" as const,color:C.muted,padding:80}}>
      <RefreshCw size={24} style={{marginBottom:12,opacity:0.4,animation:"spin 1s linear infinite"}}/>
      <div>Connecting…</div>
    </div>
  );
  const anyDeg=latest.shards.some(s=>s.status!=="healthy");
  return(
    <div style={{padding:"28px 28px 48px"}}>
      {anyDeg&&(
        <div style={{background:C.amber+"18",border:`1px solid ${C.amber}44`,borderRadius:10,padding:"12px 18px",marginBottom:20,display:"flex",alignItems:"center",gap:10,fontSize:13,color:C.amber}}>
          <AlertTriangle size={15}/> One or more shards are degraded.
        </div>
      )}
      <div style={{display:"grid",gridTemplateColumns:"repeat(4,1fr)",gap:14,marginBottom:20}}>
        <StatCard icon={Database} label="Total Vectors"  value={fmt(latest.totalVectors)} sub="real from HNSW index" accent/>
        <StatCard icon={Zap}      label="Total Inserts"  value={fmt(latest.totalInserts)}  sub="since server start"/>
        <StatCard icon={Search}   label="Total Searches" value={fmt(latest.totalSearches)} sub="since server start"/>
        <StatCard icon={Activity} label="P99 Latency"    value={fmtUs(latest.p99Us)}       sub="last search measured"/>
      </div>
      <div style={{display:"grid",gridTemplateColumns:"2fr 1fr",gap:14,marginBottom:14}}>
        <LatencyChart history={history}/>
        <div style={{display:"flex",flexDirection:"column" as const,gap:14}}>
          <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"18px 22px"}}>
            <div style={{fontSize:11,color:C.muted,textTransform:"uppercase" as const,letterSpacing:1,marginBottom:14}}>Percentiles (live)</div>
            {([["P50",latest.p50Us,C.c2],["P95",latest.p95Us,C.c1],["P99",latest.p99Us,C.c3]] as [string,number,string][]).map(([l,v,c])=>(
              <div key={l} style={{marginBottom:12}}>
                <div style={{display:"flex",justifyContent:"space-between",marginBottom:4}}>
                  <span style={{fontSize:12,color:C.muted}}>{l}</span>
                  <span style={{fontSize:12,fontWeight:600,color:c}}>{fmtUs(v)}</span>
                </div>
                <div style={{height:3,background:C.border,borderRadius:2}}>
                  <div style={{height:"100%",borderRadius:2,background:c,width:`${Math.min(100,v>0?(v/2000)*100:0)}%`,transition:"width 0.6s ease"}}/>
                </div>
              </div>
            ))}
          </div>
          <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:"18px 22px"}}>
            <div style={{fontSize:11,color:C.muted,textTransform:"uppercase" as const,letterSpacing:1,marginBottom:14}}>System Health</div>
            {latest.shards.map(s=>(
              <div key={s.id} style={{display:"flex",alignItems:"center",gap:10,marginBottom:10}}>
                {s.status==="healthy"?<CheckCircle size={14} color={C.green}/>:<AlertTriangle size={14} color={C.amber}/>}
                <span style={{fontSize:13,color:C.text}}>Shard {s.id}</span>
                <span style={{fontSize:11,color:C.muted,marginLeft:"auto"}}>{fmt(s.vectors)} vecs</span>
              </div>
            ))}
          </div>
        </div>
      </div>
      <div style={{display:"grid",gridTemplateColumns:"1fr 1fr",gap:14}}>
        <ThroughputChart history={history}/>
        <div style={{display:"grid",gap:14}}>
          <VectorGrowthChart history={history}/>
          <LoadDistChart latest={latest}/>
        </div>
      </div>
    </div>
  );
}

// ── SHARDS PAGE ───────────────────────────────────────────────────────────────

function ShardsPage({latest}:{latest:MetricsSnapshot|null}){
  if(!latest)return<div style={{textAlign:"center" as const,color:C.muted,padding:80}}>Waiting for data…</div>;
  const colors=[C.accent,C.green,C.c3];
  return(
    <div style={{padding:"28px 28px 48px"}}>
      <h2 style={{fontSize:18,fontWeight:700,color:C.text,marginBottom:6}}>Shard Map</h2>
      <p style={{fontSize:13,color:C.muted,marginBottom:24}}>{latest.shards.length} shards · {fmt(latest.totalVectors)} vectors (real)</p>
      <div style={{display:"grid",gridTemplateColumns:"repeat(3,1fr)",gap:16,marginBottom:28}}>
        {latest.shards.map(s=><ShardCard key={s.id} shard={s} total={latest.totalVectors}/>)}
      </div>
      <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24}}>
        <div style={{display:"flex",alignItems:"center",gap:8,marginBottom:20}}>
          <Cpu size={15} color={C.c3}/><span style={{fontWeight:600,color:C.text,fontSize:14}}>Consistent Hash Ring</span>
        </div>
        <svg viewBox="0 0 400 400" style={{width:"100%",maxWidth:380,display:"block",margin:"0 auto"}}>
          <circle cx="200" cy="200" r="150" fill="none" stroke={C.border} strokeWidth="32"/>
          {latest.shards.map((s,i)=>{
            const sa=(i*120-90)*(Math.PI/180),ea=((i+1)*120-90)*(Math.PI/180),r=150;
            const x1=200+r*Math.cos(sa),y1=200+r*Math.sin(sa),x2=200+r*Math.cos(ea),y2=200+r*Math.sin(ea);
            const color=colors[i%colors.length];
            const lp=latest.totalVectors>0?s.vectors/latest.totalVectors:1/latest.shards.length;
            const la=sa+lp*120*(Math.PI/180);
            const lx2=200+r*Math.cos(la),ly2=200+r*Math.sin(la);
            const ma=sa+60*(Math.PI/180);
            return(
              <g key={s.id}>
                <path d={`M ${x1} ${y1} A ${r} ${r} 0 0 1 ${x2} ${y2}`} fill="none" stroke={color+"30"} strokeWidth="28"/>
                <path d={`M ${x1} ${y1} A ${r} ${r} 0 0 1 ${lx2} ${ly2}`} fill="none" stroke={color} strokeWidth="28" opacity={0.85}/>
                <text x={200+185*Math.cos(ma)} y={200+185*Math.sin(ma)} fill={color} fontSize="13" fontWeight="700" textAnchor="middle" dominantBaseline="middle">S{s.id}</text>
              </g>
            );
          })}
          <circle cx="200" cy="200" r="90" fill={C.bg} stroke={C.border} strokeWidth="1"/>
          <text x="200" y="192" fill={C.text} fontSize="20" fontWeight="700" textAnchor="middle" dominantBaseline="middle">{fmt(latest.totalVectors)}</text>
          <text x="200" y="215" fill={C.muted} fontSize="11" textAnchor="middle">real vectors</text>
        </svg>
        <div style={{display:"flex",justifyContent:"center",gap:24,marginTop:16}}>
          {latest.shards.map((s,i)=>(
            <div key={s.id} style={{display:"flex",alignItems:"center",gap:6}}>
              <div style={{width:10,height:10,borderRadius:2,background:colors[i%colors.length]}}/>
              <span style={{fontSize:12,color:C.muted}}>S{s.id} — {latest.totalVectors>0?((s.vectors/latest.totalVectors)*100).toFixed(1):0}%</span>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

// ── SETTINGS PAGE ─────────────────────────────────────────────────────────────

function SettingsPage({serverBase,setServerBase}:{serverBase:string;setServerBase:(s:string)=>void}){
  const [draft,setDraft]=useState(serverBase);
  return(
    <div style={{padding:"28px 28px 48px",maxWidth:600}}>
      <h2 style={{fontSize:18,fontWeight:700,color:C.text,marginBottom:6}}>Settings</h2>
      <p style={{fontSize:13,color:C.muted,marginBottom:24}}>Configure the server connection.</p>
      <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24,marginBottom:16}}>
        <label style={{display:"block",fontSize:12,color:C.muted,marginBottom:8}}>HTTP API Base URL</label>
        <div style={{display:"flex",gap:10}}>
          <input value={draft} onChange={e=>setDraft(e.target.value)}
            style={{flex:1,background:C.bg,border:`1px solid ${C.border}`,borderRadius:8,color:C.text,padding:"10px 14px",fontSize:13,fontFamily:"monospace",outline:"none"}}
          />
          <button onClick={()=>setServerBase(draft)} style={{background:C.accent,color:C.bg,border:"none",borderRadius:8,padding:"10px 20px",fontSize:13,fontWeight:700,cursor:"pointer"}}>Apply</button>
        </div>
        <p style={{fontSize:11,color:C.muted,marginTop:10}}>Default: http://localhost:8080</p>
      </div>
      <div style={{background:C.surface,border:`1px solid ${C.border}`,borderRadius:12,padding:24}}>
        <div style={{fontSize:13,fontWeight:600,color:C.text,marginBottom:14}}>API Endpoints</div>
        {([["GET","/api/health","Health check"],["GET","/api/info","Shard info"],["GET","/api/metrics","Live metrics (1s)"],["POST","/api/search","ANN search"],["POST","/api/insert","Insert a vector"],["GET","/api/node?id=N&shard=S","HNSW graph inspector"]] as [string,string,string][]).map(([m,p,d])=>(
          <div key={p} style={{display:"flex",gap:12,marginBottom:10,alignItems:"baseline"}}>
            <span style={{fontSize:10,fontWeight:700,color:m==="GET"?C.green:C.accent,background:(m==="GET"?C.green:C.accent)+"22",padding:"2px 6px",borderRadius:4,minWidth:40,textAlign:"center" as const}}>{m}</span>
            <code style={{fontSize:11,color:C.text,fontFamily:"monospace"}}>{p}</code>
            <span style={{fontSize:12,color:C.muted}}>{d}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

// ── NAV ───────────────────────────────────────────────────────────────────────

type Page = "overview"|"shards"|"insert"|"search"|"graph"|"settings";

function Nav({page,setPage,connected,serverBase,onConnect}:
  {page:Page;setPage:(p:Page)=>void;connected:boolean;serverBase:string;onConnect:()=>void}){
  const tabs:{id:Page;label:string;icon:React.ElementType}[]=[
    {id:"overview",label:"Overview",icon:Activity},
    {id:"shards",  label:"Shard Map",icon:Server},
    {id:"insert",  label:"Insert",icon:Plus},
    {id:"search",  label:"Search",icon:Search},
    {id:"graph",   label:"Graph",icon:GitBranch},
    {id:"settings",label:"Settings",icon:Settings},
  ];
  return(
    <nav style={{background:C.surface,borderBottom:`1px solid ${C.border}`,display:"flex",alignItems:"center",padding:"0 24px",height:56,position:"sticky",top:0,zIndex:100}}>
      <div style={{display:"flex",alignItems:"center",gap:10,marginRight:24}}>
        <div style={{width:28,height:28,borderRadius:6,background:`linear-gradient(135deg,${C.accent},${C.c3})`,display:"flex",alignItems:"center",justifyContent:"center"}}>
          <Database size={14} color="#000"/>
        </div>
        <span style={{fontWeight:700,fontSize:15,color:C.text}}>vecdb</span>
        <span style={{fontSize:10,color:C.muted,background:C.border,padding:"1px 6px",borderRadius:4}}>v0.7</span>
      </div>
      <div style={{display:"flex",gap:2}}>
        {tabs.map(t=>(
          <button key={t.id} onClick={()=>setPage(t.id)} style={{
            display:"flex",alignItems:"center",gap:6,
            background:page===t.id?C.accent+"18":"transparent",
            border:page===t.id?`1px solid ${C.accent}44`:"1px solid transparent",
            borderRadius:8,padding:"6px 12px",color:page===t.id?C.accent:C.muted,
            fontSize:12,fontWeight:page===t.id?600:400,cursor:"pointer",
          }}><t.icon size={12}/>{t.label}</button>
        ))}
      </div>
      <div style={{marginLeft:"auto",display:"flex",alignItems:"center",gap:10}}>
        {connected&&<span style={{fontSize:11,color:C.muted,fontFamily:"monospace"}}>{serverBase.replace("http://","")}</span>}
        <div style={{width:7,height:7,borderRadius:"50%",background:connected?C.green:C.red,
          boxShadow:connected?`0 0 8px ${C.green}`:"none",
          animation:connected?"pulse 2s ease-in-out infinite":"none"}}/>
        <span style={{fontSize:12,color:C.muted}}>{connected?"Live":"Disconnected"}</span>
        {!connected&&<button onClick={onConnect} style={{background:C.accent,color:C.bg,border:"none",borderRadius:6,padding:"5px 14px",fontSize:12,fontWeight:600,cursor:"pointer"}}>Connect</button>}
      </div>
    </nav>
  );
}

// ── APP ───────────────────────────────────────────────────────────────────────

export default function App(){
  const [page,setPage]             = useState<Page>("overview");
  const [connected,setConnected]   = useState(false);
  const [serverBase,setServerBase] = useState("http://localhost:8080");
  const {history,latest,connError} = useLiveMetrics(serverBase,connected);

  return(
    <div style={{minHeight:"100vh",background:C.bg,fontFamily:"'Inter','Segoe UI',system-ui,sans-serif",color:C.text}}>
      <style>{`
        *{box-sizing:border-box;margin:0;padding:0}
        ::-webkit-scrollbar{width:6px}::-webkit-scrollbar-track{background:${C.bg}}::-webkit-scrollbar-thumb{background:${C.border};border-radius:3px}
        @keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
        @keyframes spin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
        textarea:focus,input:focus,select:focus{border-color:${C.accent}88!important}
        button:hover:not(:disabled){opacity:0.88}
      `}</style>

      <Nav page={page} setPage={setPage} connected={connected} serverBase={serverBase} onConnect={()=>setConnected(true)}/>

      {!connected&&(
        <div style={{background:`linear-gradient(135deg,${C.accent}08,${C.c3}08)`,border:`1px solid ${C.border}`,borderRadius:14,margin:"28px 28px 0",padding:"28px 32px",display:"flex",alignItems:"center",justifyContent:"space-between",gap:24}}>
          <div>
            <div style={{fontSize:18,fontWeight:700,color:C.text,marginBottom:8}}>Connect to your vecdb server</div>
            <div style={{fontSize:13,color:C.muted,lineHeight:1.7}}>
              Start: <code style={{background:C.border,padding:"1px 7px",borderRadius:4,fontSize:12,color:C.accent}}>./build/vecdb_server --no-auth --shards 3 --dim 128</code><br/>
              Then click Connect — all data is real, nothing is simulated.
            </div>
          </div>
          <button onClick={()=>setConnected(true)} style={{background:C.accent,color:C.bg,border:"none",borderRadius:10,padding:"12px 32px",fontSize:14,fontWeight:700,cursor:"pointer",whiteSpace:"nowrap"}}>Connect</button>
        </div>
      )}

      {page==="overview" &&<OverviewPage history={history} latest={latest} connError={connError}/>}
      {page==="shards"   &&<ShardsPage   latest={latest}/>}
      {page==="insert"   &&<InsertPage   serverBase={serverBase} dim={128}/>}
      {page==="search"   &&<SearchPage   serverBase={serverBase} dim={128}/>}
      {page==="graph"    &&<GraphInspectorPage serverBase={serverBase} latest={latest}/>}
      {page==="settings" &&<SettingsPage serverBase={serverBase} setServerBase={s=>{setServerBase(s);setConnected(false);}}/>}
    </div>
  );
}

