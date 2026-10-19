/*
 * Painel do horímetro — lê e escreve direto no Firestore (Firebase JS SDK).
 * Todos os cálculos (totais, períodos, status das tarefas) são feitos aqui no navegador.
 */
import { initializeApp } from 'https://www.gstatic.com/firebasejs/10.14.1/firebase-app.js';
import {
  getAuth, onAuthStateChanged, signInWithEmailAndPassword, signOut,
} from 'https://www.gstatic.com/firebasejs/10.14.1/firebase-auth.js';
import {
  getFirestore, collection, doc, onSnapshot, addDoc, setDoc, updateDoc, deleteDoc,
  query, orderBy, limit, writeBatch,
} from 'https://www.gstatic.com/firebasejs/10.14.1/firebase-firestore.js';
import { firebaseConfig } from './firebase-config.js';

const app = initializeApp(firebaseConfig);
const auth = getAuth(app);
const db = getFirestore(app);

const ONLINE_TIMEOUT_S = 90;   // heartbeat a cada 30 s

/* ---------- Utilidades ---------- */
const $ = (s) => document.querySelector(s);
const nf1 = new Intl.NumberFormat('pt-BR', { minimumFractionDigits: 1, maximumFractionDigits: 1 });
const fmtH = (h) => nf1.format(h) + ' h';
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const nowS = () => Date.now() / 1000;

function fmtDur(s) {
  s = Math.round(s);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  if (h) return `${h} h ${String(m).padStart(2, '0')} min`;
  if (m) return `${m} min ${String(sec).padStart(2, '0')} s`;
  return `${sec} s`;
}
const fmtDate = (ts) => new Date(ts * 1000).toLocaleString('pt-BR', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
const localKey = (ts) => {
  const d = new Date(ts * 1000);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
};

let toastTimer;
function toast(msg) {
  const t = $('#toast');
  t.textContent = msg; t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), 3500);
}

/* ---------- Estado (preenchido pelos listeners do Firestore) ---------- */
const state = {
  sessions: [],   // { start, duration_s }
  status: null,   // doc status/live
  tasks: [],      // docs de tasks
  log: [],        // docs de task_log
  offset: 0,      // horas iniciais
};
let unsubs = [];
let seeding = false;

function sessionStart(d) {
  if (d.start_ts > 1.7e9) return d.start_ts;                       // ESP32 com relógio sincronizado
  const created = d.created_at?.seconds;                           // senão: horário do servidor - duração
  return created ? created - d.duration_s : 0;
}

/* ---------- Cálculos ---------- */
function liveState() {
  const st = state.status;
  const updated = st?.updated_at?.seconds;
  if (!st || !updated) return { online: false, running: false, current_s: 0, last_seen: null, device: null };
  const age = nowS() - updated;
  const online = age <= ONLINE_TIMEOUT_S;
  const running = online && !!st.running;
  return {
    online, running,
    current_s: running ? (st.current_s || 0) + Math.max(0, Math.floor(age)) : 0,
    last_seen: updated,
    device: { id: st.device_id, rssi: st.rssi, fw: st.fw },
  };
}

function hoursSince(fromTs) {
  return state.sessions.reduce((a, s) => (s.start >= fromTs ? a + s.duration_s : a), 0) / 3600;
}

function periodStarts() {
  const d = new Date(); d.setHours(0, 0, 0, 0);
  const day = d.getTime() / 1000;
  const dow = (d.getDay() + 6) % 7;                                // 0 = segunda
  const week = day - dow * 86400;
  const month = new Date(d.getFullYear(), d.getMonth(), 1).getTime() / 1000;
  return { day, week, month };
}

function totalHours() {
  const recorded = state.sessions.reduce((a, s) => a + s.duration_s, 0) / 3600;
  return state.offset + recorded + liveState().current_s / 3600;
}

function taskStatus(t, total) {
  const since = Math.max(0, total - (t.last_done_hours || 0));
  const pct = since / t.interval_hours;
  const warn = t.warn_pct || 0.9;
  return {
    ...t, hours_since: since, remaining_hours: t.interval_hours - since, pct,
    status: pct >= 1 ? 'overdue' : pct >= warn ? 'warning' : 'ok',
  };
}

/* ---------- Renderização ---------- */
function renderOdometer(hours) {
  const [int, dec] = hours.toFixed(1).split('.');
  $('#odometer').innerHTML =
    int.padStart(5, '0').split('').map((d) => `<span class="digit">${d}</span>`).join('') +
    `<span class="digit tenth">${dec}</span><span class="unit">h</span>`;
  $('#odometer').setAttribute('aria-label', `${nf1.format(hours)} horas`);
}

let tasksView = [];
function renderAll() {
  const live = liveState();
  const total = totalHours();
  const p = periodStarts();
  const liveH = live.current_s / 3600;

  renderOdometer(total);
  $('#hToday').textContent = fmtH(hoursSince(p.day) + liveH);
  $('#hWeek').textContent = fmtH(hoursSince(p.week) + liveH);
  $('#hMonth').textContent = fmtH(hoursSince(p.month) + liveH);

  $('#status').className = 'status ' + (!live.online ? 'offline' : live.running ? 'running' : '');
  $('#statusText').textContent = !live.online ? 'Sem conexão com o ESP32' : live.running ? 'Em operação' : 'Parado';
  $('#sessionLive').textContent = live.running ? `Sessão atual: ${fmtDur(live.current_s)}` : '';
  $('#deviceInfo').textContent = live.device
    ? `${live.device.id} · Wi-Fi ${live.device.rssi ?? '?'} dBm · visto às ${new Date(live.last_seen * 1000).toLocaleTimeString('pt-BR')}`
    : 'Nenhum dado recebido do ESP32 ainda';

  tasksView = state.tasks.map((t) => taskStatus(t, total)).sort((a, b) => a.remaining_hours - b.remaining_hours);
  renderTasks(tasksView);
  renderBanner(tasksView);
}

function renderBanner(tasks) {
  const over = tasks.filter((t) => t.status === 'overdue');
  const warn = tasks.filter((t) => t.status === 'warning');
  const b = $('#banner');
  if (!over.length && !warn.length) { b.hidden = true; return; }
  b.hidden = false;
  b.className = 'banner' + (over.length ? '' : ' warn');
  const parts = [];
  if (over.length) parts.push(`<strong>${over.length === 1 ? '1 manutenção vencida' : over.length + ' manutenções vencidas'}</strong>${over.map((t) => esc(t.name)).join(', ')}`);
  if (warn.length) parts.push(`<strong>${warn.length === 1 ? '1 manutenção próxima do prazo' : warn.length + ' manutenções próximas do prazo'}</strong>${warn.map((t) => esc(t.name)).join(', ')}`);
  b.innerHTML = parts.join('<br>');
}

function renderTasks(tasks) {
  const el = $('#tasks');
  if (!tasks.length) { el.innerHTML = '<p class="empty">Nenhuma tarefa cadastrada. Use “Nova tarefa” para começar.</p>'; return; }
  el.innerHTML = tasks.map((t) => {
    const when = t.status === 'overdue'
      ? `Vencida há ${fmtH(-t.remaining_hours)}`
      : `Faltam ${fmtH(t.remaining_hours)} de ${fmtH(t.interval_hours)}`;
    return `<div class="task ${t.status}">
      <div class="task-top"><span class="task-name">${esc(t.name)}</span></div>
      ${t.description ? `<div class="task-desc">${esc(t.description)}</div>` : ''}
      <div class="bar" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow="${Math.min(100, Math.round(t.pct * 100))}" aria-label="Uso do intervalo"><span style="width:${Math.min(100, t.pct * 100)}%"></span></div>
      <div class="task-meta">
        <span class="when">${when}</span>
        <span class="actions">
          <button class="ghost" data-edit="${esc(t.id)}">Editar</button>
          <button data-done="${esc(t.id)}">Registrar feita</button>
        </span>
      </div>
    </div>`;
  }).join('');
}

function renderChart() {
  const N = 30;
  const todayStart = periodStarts().day;
  const buckets = {};
  state.sessions.forEach((s) => { const k = localKey(s.start); buckets[k] = (buckets[k] || 0) + s.duration_s / 3600; });
  const days = [];
  for (let i = N - 1; i >= 0; i--) {
    const d = new Date((todayStart - i * 86400) * 1000 + 3600 * 1000 * 2); // evita borda de horário de verão
    const key = localKey(d.getTime() / 1000);
    days.push({ date: key, hours: buckets[key] || 0 });
  }
  const W = 480, H = 150, padL = 26, padB = 18, padT = 8;
  const top = Math.ceil(Math.max(1, ...days.map((d) => d.hours)));
  const bw = (W - padL) / days.length;
  const lbl = (k) => k.split('-').reverse().slice(0, 2).join('/');
  let svg = `<svg class="chart" viewBox="0 0 ${W} ${H}" role="img" aria-label="Horas de uso por dia">`;
  [0, 0.5, 1].forEach((f) => {
    const y = padT + (H - padT - padB) * (1 - f);
    svg += `<line x1="${padL}" x2="${W}" y1="${y}" y2="${y}"/><text x="0" y="${y + 3}">${nf1.format(top * f)}</text>`;
  });
  days.forEach((d, i) => {
    const h = (H - padT - padB) * (d.hours / top);
    svg += `<rect class="bar-rect" x="${padL + i * bw + 1}" y="${H - padB - h}" width="${Math.max(1, bw - 2)}" height="${h}" rx="1"><title>${lbl(d.date)}: ${fmtH(d.hours)}</title></rect>`;
  });
  [0, Math.floor(N / 2), N - 1].forEach((i) => { svg += `<text x="${padL + i * bw}" y="${H - 4}">${lbl(days[i].date)}</text>`; });
  $('#chart').innerHTML = svg + '</svg>';
}

function renderSessions() {
  const rows = [...state.sessions].sort((a, b) => b.start - a.start).slice(0, 15);
  $('#sessions').innerHTML = rows.length
    ? `<table><thead><tr><th>Início</th><th>Duração</th></tr></thead><tbody>${rows.map((r) => `<tr><td>${fmtDate(r.start)}</td><td>${fmtDur(r.duration_s)}</td></tr>`).join('')}</tbody></table>`
    : '<p class="empty">Nenhuma sessão registrada. Ligue o torno para gerar a primeira.</p>';
}

function renderLog() {
  const rows = state.log;
  $('#log').innerHTML = rows.length
    ? `<table><thead><tr><th>Tarefa</th><th>Quando</th></tr></thead><tbody>${rows.map((r) => `<tr><td>${esc(r.task_name)}${r.note ? `<br><small>${esc(r.note)}</small>` : ''}</td><td>${fmtDate(r.done_at)}<br><small>${fmtH(r.at_hours)}</small></td></tr>`).join('')}</tbody></table>`
    : '<p class="empty">Nenhuma manutenção registrada ainda.</p>';
}

/* ---------- Listeners do Firestore ---------- */
function onError(e) {
  console.error(e);
  if (e.code === 'permission-denied') {
    toast('Sem permissão: seu e-mail não está liberado em firestore.rules.');
  } else {
    toast('Erro ao ler dados: ' + (e.message || e.code));
  }
}

const DEFAULT_TASKS = [
  ['Lubrificar barramento e guias', '[EXEMPLO] Confirme intervalo e lubrificante no manual Nardini MS175.', 8],
  ['Inspecionar correias e fixações', '[EXEMPLO] Confirme intervalo no manual.', 100],
  ['Trocar óleo do cabeçote', '[EXEMPLO] Confirme intervalo e tipo de óleo no manual.', 500],
  ['Revisão geral', '[EXEMPLO] Confirme intervalo no manual.', 1000],
];

async function seedTasksOnce() {
  if (seeding) return;
  seeding = true;
  try {
    const batch = writeBatch(db);
    DEFAULT_TASKS.forEach(([name, description, interval_hours]) => {
      batch.set(doc(collection(db, 'tasks')), { name, description, interval_hours, warn_pct: 0.9, last_done_hours: 0, last_done_at: null });
    });
    batch.set(doc(db, 'settings', 'config'), { seeded: true }, { merge: true });
    await batch.commit();
  } catch (e) { onError(e); }
}

function startListening() {
  stopListening();
  unsubs.push(onSnapshot(collection(db, 'sessions'), (snap) => {
    state.sessions = snap.docs.map((d) => {
      const x = d.data();
      return { start: sessionStart(x), duration_s: Number(x.duration_s) || 0 };
    });
    renderAll(); renderChart(); renderSessions();
  }, onError));

  unsubs.push(onSnapshot(doc(db, 'status', 'live'), (snap) => {
    state.status = snap.exists() ? snap.data() : null;
    renderAll();
  }, onError));

  unsubs.push(onSnapshot(collection(db, 'tasks'), (snap) => {
    state.tasks = snap.docs.map((d) => ({ id: d.id, ...d.data() }));
    renderAll();
  }, onError));

  unsubs.push(onSnapshot(doc(db, 'settings', 'config'), (snap) => {
    const cfg = snap.exists() ? snap.data() : {};
    state.offset = Number(cfg.offset_hours) || 0;
    if (!cfg.seeded) seedTasksOnce();   // primeira execução: cria as tarefas de exemplo
    renderAll();
  }, onError));

  unsubs.push(onSnapshot(query(collection(db, 'task_log'), orderBy('done_at', 'desc'), limit(15)), (snap) => {
    state.log = snap.docs.map((d) => d.data());
    renderLog();
  }, onError));
}

function stopListening() { unsubs.forEach((u) => u()); unsubs = []; }

/* ---------- Diálogos e ações ---------- */
let editingId = null, doneId = null;

function openTaskDlg(t) {
  editingId = t ? t.id : null;
  $('#taskDlgTitle').textContent = t ? 'Editar tarefa' : 'Nova tarefa';
  $('#fName').value = t ? t.name : '';
  $('#fDesc').value = t ? t.description || '' : '';
  $('#fInterval').value = t ? t.interval_hours : '';
  $('#fWarn').value = t ? Math.round((t.warn_pct || 0.9) * 100) : 90;
  $('#fDelete').style.display = t ? '' : 'none';
  $('#taskErr').textContent = '';
  $('#taskDlg').showModal();
}

$('#addTask').onclick = () => openTaskDlg(null);
$('#fCancel').onclick = () => $('#taskDlg').close();
$('#dCancel').onclick = () => $('#doneDlg').close();

$('#taskForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const name = $('#fName').value.trim();
  const interval = Number($('#fInterval').value);
  const warn = Number($('#fWarn').value) / 100;
  if (!name) { $('#taskErr').textContent = 'Informe o nome da tarefa.'; return; }
  if (!(interval > 0)) { $('#taskErr').textContent = 'O intervalo deve ser maior que zero.'; return; }
  if (!(warn > 0 && warn < 1)) { $('#taskErr').textContent = 'O aviso deve ficar entre 1% e 99%.'; return; }
  const data = { name, description: $('#fDesc').value.trim(), interval_hours: interval, warn_pct: warn };
  try {
    if (editingId) await updateDoc(doc(db, 'tasks', editingId), data);
    else await addDoc(collection(db, 'tasks'), { ...data, last_done_hours: totalHours(), last_done_at: null });
    $('#taskDlg').close(); toast('Tarefa salva');
  } catch (err) { $('#taskErr').textContent = err.message; }
});

$('#fDelete').onclick = async () => {
  if (!confirm('Excluir esta tarefa de manutenção?')) return;
  try { await deleteDoc(doc(db, 'tasks', editingId)); $('#taskDlg').close(); toast('Tarefa excluída'); }
  catch (err) { $('#taskErr').textContent = err.message; }
};

$('#doneForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const t = state.tasks.find((x) => x.id === doneId);
  if (!t) return;
  const total = totalHours();
  try {
    const batch = writeBatch(db);
    batch.update(doc(db, 'tasks', doneId), { last_done_hours: total, last_done_at: Math.floor(nowS()) });
    batch.set(doc(collection(db, 'task_log')), {
      task_id: doneId, task_name: t.name, done_at: Math.floor(nowS()), at_hours: total,
      note: $('#dNote').value.trim().slice(0, 300),
    });
    await batch.commit();
    $('#doneDlg').close(); toast('Manutenção registrada');
  } catch (err) { $('#doneErr').textContent = err.message; }
});

$('#tasks').addEventListener('click', (e) => {
  const ed = e.target.closest('[data-edit]'), dn = e.target.closest('[data-done]');
  if (ed) openTaskDlg(state.tasks.find((t) => t.id === ed.dataset.edit));
  if (dn) {
    doneId = dn.dataset.done;
    $('#doneTitle').textContent = 'Registrar: ' + state.tasks.find((t) => t.id === doneId).name;
    $('#dNote').value = ''; $('#doneErr').textContent = '';
    $('#doneDlg').showModal();
  }
});

$('#editOffset').onclick = async () => {
  const v = prompt('Horas que o torno já tinha antes da instalação do sistema:', String(state.offset).replace('.', ','));
  if (v === null) return;
  const h = Number(v.replace(',', '.'));
  if (!Number.isFinite(h) || h < 0) { alert('Informe um valor de horas válido (0 ou mais).'); return; }
  try { await setDoc(doc(db, 'settings', 'config'), { offset_hours: h }, { merge: true }); toast('Horas iniciais atualizadas'); }
  catch (err) { alert(err.message); }
};

/* ---------- Login ---------- */
const AUTH_ERRORS = {
  'auth/invalid-credential': 'E-mail ou senha incorretos.',
  'auth/invalid-email': 'E-mail inválido.',
  'auth/too-many-requests': 'Muitas tentativas. Aguarde alguns minutos e tente de novo.',
  'auth/network-request-failed': 'Sem conexão com a internet.',
};

$('#loginForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  $('#loginErr').textContent = '';
  try {
    await signInWithEmailAndPassword(auth, $('#lEmail').value.trim(), $('#lPass').value);
    $('#lPass').value = '';
  } catch (err) {
    $('#loginErr').textContent = AUTH_ERRORS[err.code] || 'Não foi possível entrar: ' + err.code;
  }
});
$('#loginDlg').addEventListener('cancel', (e) => e.preventDefault());   // login é obrigatório
$('#logout').onclick = () => signOut(auth);

onAuthStateChanged(auth, (user) => {
  if (user) {
    $('#loginDlg').close();
    $('#logout').hidden = false;
    startListening();
  } else {
    stopListening();
    $('#logout').hidden = true;
    if (!$('#loginDlg').open) $('#loginDlg').showModal();
  }
});

// Atualiza "online/offline" e o contador da sessão atual sem esperar novos dados
setInterval(() => { if (auth.currentUser) renderAll(); }, 5000);
