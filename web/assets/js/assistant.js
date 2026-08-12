import {requestJson} from './api.js';
import {categoryColor} from './chart_palette.js';
import {mountFocusTimer, syncFocusDurationField} from './focus_timer.js';

const state = {documents: [], filter: 'all'};
let focusTimerController;
const scheduleState = {
  month: new Date(new Date().getFullYear(), new Date().getMonth(), 1),
  selectedDateKey: localDateKey(new Date()),
  events: [],
  tasks: [],
  focusRecords: [],
  checkinRecords: [],
  projects: [],
  insights: {projects: [], entries: [], total_elapsed_seconds: 0}
};
const $ = selector => document.querySelector(selector);
const $$ = selector => [...document.querySelectorAll(selector)];

async function api(path, options = {}) {
  return requestJson(path, options);
}

function showView(name) {
  $$('.view').forEach(view => view.classList.toggle('active', view.id === `${name}-view`));
  $$('#nav button').forEach(button => button.classList.toggle('active', button.dataset.view === name));
  const activeNavigation = $(`#nav button[data-view="${name}"]`);
  if (window.innerWidth <= 768) {
    activeNavigation.scrollIntoView({block: 'nearest', inline: 'center'});
  }
  const titles = {
    home: '今天，先把重要的事理清楚。',
    knowledge: '把资料变成可以追溯的知识。',
    schedule: '计划和完成，都落在同一条时间线上。',
    ledger: '每一笔都有准确的去向。',
    health: '用趋势理解自己的状态。'
  };
  $('#view-title').textContent = titles[name];
  if (name === 'knowledge') {
    loadDocuments();
  }
  window.loadLifeView?.(name);
}

$$('#nav button').forEach(button => {
  button.onclick = () => showView(button.dataset.view);
});
$$('[data-jump]').forEach(button => {
  button.onclick = () => showView(button.dataset.jump);
});

async function loadDocuments() {
  try {
    const data = await api('/api/documents');
    state.documents = data.documents;
    renderDocuments();
    $('#document-count').textContent = state.documents.length;
    $('#review-count').textContent = state.documents.filter(item => item.learning_status === 'needs_review').length;
  } catch (error) {
    $('#documents').innerHTML = `<div class="card empty error">${escapeHtml(error.message)}</div>`;
  }
}

function renderDocuments() {
  const visible = state.filter === 'all' ? state.documents : state.documents.filter(item => item.learning_status === state.filter);
  if (!visible.length) {
    $('#documents').innerHTML = '<div class="card empty">当前分类还没有文档。</div>';
    return;
  }
  $('#documents').innerHTML = visible.map(item => `<article class="card file-card"><p class="eyebrow">${item.status.toUpperCase()}</p><h3 title="${escapeHtml(item.filename)}">${escapeHtml(item.filename)}</h3><p class="meta">${formatBytes(item.size_bytes)} · ${escapeHtml(item.media_type)}</p><a class="source-link" href="/api/documents/${item.id}/content" target="_blank">打开原件</a><select data-status-id="${item.id}"><option value="unreviewed">未整理</option><option value="learning">学习中</option><option value="mastered">已掌握</option><option value="needs_review">待复习</option></select></article>`).join('');
  $$('[data-status-id]').forEach(select => {
    const item = state.documents.find(row => row.id === Number(select.dataset.statusId));
    select.value = item.learning_status;
    select.onchange = () => updateStatus(item.id, select.value);
  });
}

function uploadErrorMessage(code) {
  const messages = {
    network_unavailable: '上传请求未能发出，请检查服务是否正在运行。',
    unsupported_media_type: '仅支持 TXT、Markdown、PDF 和 DOCX 文件。',
    document_too_large: '文件不能超过 8 MiB。',
    invalid_filename: '文件名包含不支持的字符。',
    document_submission_unavailable: '文件已保存，但后台索引服务暂时不可用。'
  };
  return messages[code] || `上传失败：${code}`;
}

async function updateStatus(id, status) {
  await api(`/api/knowledge/${id}/status`, {
    method: 'PATCH',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({status})
  });
  const item = state.documents.find(row => row.id === id);
  item.learning_status = status;
  renderDocuments();
}

$$('[data-filter]').forEach(button => {
  button.onclick = () => {
    state.filter = button.dataset.filter;
    $$('[data-filter]').forEach(item => item.classList.toggle('active', item === button));
    renderDocuments();
  };
});

$('#file-input').onchange = async event => {
  const file = event.target.files[0];
  if (!file) {
    return;
  }
  const extension = file.name.split('.').pop().toLowerCase();
  const mediaTypes = {
    txt: 'text/plain',
    md: 'text/markdown',
    pdf: 'application/pdf',
    docx: 'application/vnd.openxmlformats-officedocument.wordprocessingml.document'
  };
  const media = file.type || mediaTypes[extension];
  const notice = $('#upload-state');
  notice.hidden = false;
  notice.classList.remove('error');
  notice.textContent = '正在安全保存并建立索引…';
  try {
    await api('/api/documents', {
      method: 'POST',
      headers: {'Content-Type': media, 'X-QAI-Filename': encodeURIComponent(file.name)},
      body: await file.arrayBuffer()
    });
    notice.textContent = '上传成功，后台正在提取文本。';
    await loadDocuments();
    window.setTimeout(loadDocuments, 1200);
  } catch (error) {
    notice.classList.add('error');
    notice.textContent = uploadErrorMessage(error.message);
  }
  event.target.value = '';
};

$('#ask-form').onsubmit = async event => {
  event.preventDefault();
  const panel = $('#answer');
  panel.className = 'answer';
  panel.textContent = '正在检索本地资料…';
  try {
    const result = await api('/api/knowledge/ask', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({query: $('#ask-query').value})
    });
    const evidence = result.evidence.map(item => `<div class="evidence"><a class="source-link" target="_blank" href="/api/documents/${item.document_id}/content">${escapeHtml(item.filename)}${item.page ? ` · 第 ${item.page} 页` : ''}</a><p>${escapeHtml(item.excerpt)}</p></div>`).join('');
    panel.innerHTML = `<p>${escapeHtml(result.answer)}</p>${evidence}`;
  } catch (error) {
    panel.innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
  }
};

$('#logout').onclick = async () => {
  await fetch('/api/logout', {method: 'POST'});
  location = '/login';
};

function escapeHtml(value) {
  return String(value).replace(/[&<>"]/g, character => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[character]));
}

function formatBytes(bytes) {
  if (bytes < 1024) {
    return `${bytes} B`;
  }
  if (bytes < 1048576) {
    return `${(bytes / 1024).toFixed(1)} KB`;
  }
  return `${(bytes / 1048576).toFixed(1)} MB`;
}

async function lifeRecords(domain) {
  const response = await api(`/api/life/${domain}`);
  return response.records;
}

function dateInput(ms) {
  return new Date(ms).toISOString().slice(0, 16);
}

function localDateKey(date) {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  return `${year}-${month}-${day}`;
}

function dateFromKey(dateKey) {
  const [year, month, day] = dateKey.split('-').map(Number);
  return new Date(year, month - 1, day);
}

function rangeForDay(date) {
  const start = new Date(date.getFullYear(), date.getMonth(), date.getDate());
  const end = new Date(date.getFullYear(), date.getMonth(), date.getDate() + 1);
  return {range_start_ms: start.getTime(), range_end_ms: end.getTime()};
}

function rangeForMonth(date) {
  const start = new Date(date.getFullYear(), date.getMonth(), 1);
  const end = new Date(date.getFullYear(), date.getMonth() + 1, 1);
  return {range_start_ms: start.getTime(), range_end_ms: end.getTime()};
}

async function activityInsights(range) {
  return api('/api/life/activity/insights', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(range)
  });
}

function modal(title, fields, onSubmit) {
  $('#modal-content').innerHTML = `<h2>${title}</h2>${fields}`;
  const dialog = $('#modal');
  dialog.showModal();
  $('#modal-form .close').onclick = () => dialog.close();
  $('#modal-form').onsubmit = async event => {
    event.preventDefault();
    await onSubmit(new FormData(event.currentTarget));
    dialog.close();
  };
}

function projectType(project) {
  return project.data.project_type || 'legacy';
}

function projectsForType(projects, type) {
  const unique = new Map();
  projects.filter(project => !project.data.archived).forEach(project => {
    const currentType = projectType(project);
    if (currentType !== 'legacy' && currentType !== type) {
      return;
    }
    const name = project.data.name.trim().toLocaleLowerCase('zh-CN');
    const key = `${currentType}:${name}`;
    if (!unique.has(key)) {
      unique.set(key, project);
    }
  });
  return [...unique.values()];
}

function renderProjectActions(projects) {
  const todayKey = localDateKey(new Date());
  const checked = new Set(scheduleState.checkinRecords.filter(record => record.data.local_date === todayKey).map(record => record.data.habit_id));
  const render = (type, target) => {
    const cards = projectsForType(projects, type).map(project => {
      const completed = type === 'checkin' && checked.has(project.id);
      const label = completed ? '已完成' : type === 'focus' ? '开始专注' : '立即打卡';
      return `<button class="project-action ${completed ? 'completed' : ''}" data-project-action="${type}" data-project-id="${project.id}" style="--project-color:${project.data.color || '#5E7C6B'}" ${completed ? 'disabled' : ''}><span class="project-icon">${escapeHtml(project.data.icon || '●')}</span><span><strong>${escapeHtml(project.data.name)}</strong><small>${label}</small></span></button>`;
    });
    $(target).innerHTML = cards.join('') || `<p class="empty">还没有${type === 'focus' ? '专注' : '打卡'}项目。</p>`;
  };
  render('focus', '#focus-projects');
  render('checkin', '#checkin-projects');
  $$('[data-project-action="focus"]').forEach(button => {
    button.onclick = () => openFocusModal(projectsForType(projects, 'focus'), Number(button.dataset.projectId));
  });
  $$('[data-project-action="checkin"]').forEach(button => {
    button.onclick = async () => {
      await api('/api/life/checkins', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({habit_id: Number(button.dataset.projectId), local_date: todayKey})});
      await loadSchedule();
    };
  });
}

function row(record, detail) {
  return `<div class="record-row"><div><strong>${escapeHtml(detail)}</strong><small>${new Date(record.occurred_at_ms).toLocaleString()}</small></div><button class="danger" data-delete-domain="${record.domain}" data-delete-id="${record.id}">删除</button></div>`;
}

function bindDeletes(reload) {
  $$('[data-delete-id]').forEach(button => {
    button.onclick = async () => {
      if (!confirm('确定删除这条记录吗？')) {
        return;
      }
      await api(`/api/life/${button.dataset.deleteDomain}/${button.dataset.deleteId}`, {method: 'DELETE'});
      reload();
    };
  });
}

function eventDateKey(record) {
  return localDateKey(new Date(record.data.starts_at_ms));
}

function taskDateKey(record) {
  return localDateKey(new Date(record.data.due_at_ms));
}

function entryDateKey(entry) {
  if (entry.kind === 'checkin' && entry.local_date) {
    return entry.local_date;
  }
  return localDateKey(new Date(entry.occurred_at_ms));
}

function selectedActivityEntries() {
  return scheduleState.insights.entries.filter(entry => entryDateKey(entry) === scheduleState.selectedDateKey);
}

function dayHasContent(dateKey) {
  const hasEvent = scheduleState.events.some(record => eventDateKey(record) === dateKey);
  const hasTask = scheduleState.tasks.some(record => taskDateKey(record) === dateKey);
  const hasActivity = scheduleState.insights.entries.some(entry => entryDateKey(entry) === dateKey);
  return hasEvent || hasTask || hasActivity;
}

function selectedDayInsights(entries, projectsById) {
  const totals = new Map();
  entries.filter(entry => entry.kind === 'focus').forEach(entry => {
    const elapsed = totals.get(entry.project_id) || 0;
    totals.set(entry.project_id, elapsed + entry.elapsed_seconds);
  });
  const projects = [...totals.entries()].map(([projectId, elapsedSeconds]) => {
    const project = projectsById.get(projectId) || {id: projectId, name: '未分类', color: '#8B8F8C'};
    return {...project, elapsed_seconds: elapsedSeconds};
  });
  const totalElapsedSeconds = projects.reduce((total, project) => total + project.elapsed_seconds, 0);
  return {projects, total_elapsed_seconds: totalElapsedSeconds};
}

function formatDuration(seconds) {
  if (seconds <= 0) {
    return '0 分钟';
  }
  if (seconds < 60) {
    return '不足 1 分钟';
  }
  return `${Math.round(seconds / 60)} 分钟`;
}

function renderActivityChart(insights) {
  const chart = $('#activity-chart');
  const projects = insights.projects.filter(project => project.elapsed_seconds > 0);
  $('#activity-total').textContent = formatDuration(insights.total_elapsed_seconds);
  if (!projects.length) {
    chart.innerHTML = '<div class="donut empty-donut" aria-label="当天暂无专注记录"></div><p class="empty">当天还没有完成的专注记录。</p>';
    return;
  }
  let offset = 0;
  const segments = projects.map(project => {
    const start = offset;
    offset += project.elapsed_seconds / insights.total_elapsed_seconds * 100;
    return `${project.color} ${start.toFixed(2)}% ${offset.toFixed(2)}%`;
  });
  const legend = projects.map(project => `<div class="legend-row"><i style="--project-color:${project.color}"></i><span>${escapeHtml(project.name)}</span><strong>${formatDuration(project.elapsed_seconds)}</strong></div>`).join('');
  chart.innerHTML = `<div class="donut" style="--segments:${segments.join(',')}" aria-label="选中日期专注时间分布"></div><div class="donut-legend">${legend}</div>`;
}

function scheduleGroup(title, items, emptyText) {
  const content = items.length ? items.join('') : `<p class="empty">${emptyText}</p>`;
  return `<section class="day-group"><h4>${title}</h4>${content}</section>`;
}

function renderSelectedDay() {
  const dateKey = scheduleState.selectedDateKey;
  const selectedDate = dateFromKey(dateKey);
  const dateTitle = selectedDate.toLocaleDateString('zh-CN', {month: 'long', day: 'numeric', weekday: 'long'});
  const events = scheduleState.events.filter(record => eventDateKey(record) === dateKey);
  const tasks = scheduleState.tasks.filter(record => taskDateKey(record) === dateKey);
  const entries = selectedActivityEntries();
  const focusEntries = entries.filter(entry => entry.kind === 'focus');
  const checkinEntries = entries.filter(entry => entry.kind === 'checkin');
  const projectsById = new Map(scheduleState.insights.projects.map(project => [project.id, project]));
  $('#schedule-date-title').textContent = dateTitle;
  renderActivityChart(selectedDayInsights(entries, projectsById));

  const taskItems = tasks.map(record => `<div class="day-entry"><i class="entry-mark todo-mark"></i><div><strong>${escapeHtml(record.data.title)}</strong><small>${record.data.completed ? '已完成' : '待完成'} · ${new Date(record.data.due_at_ms).toLocaleTimeString('zh-CN', {hour: '2-digit', minute: '2-digit'})}</small></div><button class="danger" data-delete-domain="tasks" data-delete-id="${record.id}">删除</button></div>`);
  const eventItems = events.map(record => `<div class="day-entry"><i class="entry-mark event-mark"></i><div><strong>${escapeHtml(record.data.title)}</strong><small>${new Date(record.data.starts_at_ms).toLocaleTimeString('zh-CN', {hour: '2-digit', minute: '2-digit'})}–${new Date(record.data.ends_at_ms).toLocaleTimeString('zh-CN', {hour: '2-digit', minute: '2-digit'})}</small></div><button class="danger" data-delete-domain="calendar" data-delete-id="${record.id}">删除</button></div>`);
  const focusItems = focusEntries.map(entry => {
    const project = projectsById.get(entry.project_id) || {name: '未分类', color: '#8B8F8C'};
    return `<div class="day-entry"><i class="entry-mark" style="--entry-color:${project.color}"></i><div><strong>${escapeHtml(project.name)}</strong><small>完成 ${formatDuration(entry.elapsed_seconds)}专注</small></div></div>`;
  });
  const checkinItems = checkinEntries.map(entry => {
    const project = projectsById.get(entry.project_id) || {name: '未分类', color: '#8B8F8C'};
    return `<div class="day-entry"><i class="entry-mark" style="--entry-color:${project.color}"></i><div><strong>${escapeHtml(project.name)}</strong><small>已打卡</small></div></div>`;
  });
  const groups = [
    scheduleGroup('待办', taskItems, '当天没有待办'),
    scheduleGroup('日历事项', eventItems, '当天没有事项'),
    scheduleGroup('专注', focusItems, '当天没有完成专注'),
    scheduleGroup('打卡', checkinItems, '当天没有打卡')
  ];
  $('#schedule-day-detail').innerHTML = groups.join('');
  bindDeletes(loadSchedule);
}

function selectScheduleDate(dateKey) {
  scheduleState.selectedDateKey = dateKey;
  renderScheduleCalendar();
  renderSelectedDay();
}

function renderScheduleCalendar() {
  const calendar = $('#schedule-calendar');
  const monthStart = scheduleState.month;
  const monthEnd = new Date(monthStart.getFullYear(), monthStart.getMonth() + 1, 0);
  const leading = (monthStart.getDay() + 6) % 7;
  const todayKey = localDateKey(new Date());
  $('#schedule-month').textContent = `${monthStart.getFullYear()} 年 ${monthStart.getMonth() + 1} 月`;
  calendar.replaceChildren();
  for (let index = 0; index < leading; index += 1) {
    const spacer = document.createElement('span');
    spacer.className = 'calendar-day outside';
    calendar.appendChild(spacer);
  }
  for (let day = 1; day <= monthEnd.getDate(); day += 1) {
    const date = new Date(monthStart.getFullYear(), monthStart.getMonth(), day);
    const dateKey = localDateKey(date);
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'calendar-day';
    button.classList.toggle('today', dateKey === todayKey);
    button.classList.toggle('selected', dateKey === scheduleState.selectedDateKey);
    button.setAttribute('aria-label', `${dateKey}${dayHasContent(dateKey) ? '，有安排' : '，无安排'}`);
    const number = document.createElement('span');
    number.textContent = String(day);
    button.appendChild(number);
    if (dayHasContent(dateKey)) {
      const dot = document.createElement('i');
      dot.className = 'calendar-content-dot';
      button.appendChild(dot);
    }
    button.addEventListener('click', () => selectScheduleDate(dateKey));
    calendar.appendChild(button);
  }
}

function focusRecordLabel(record) {
  const mode = (record.data.timer_mode || 'countdown') === 'stopwatch' ? '正计时' : `${record.data.planned_minutes} 分钟`;
  return `${mode} · ${record.data.status}`;
}

function openFocusModal(projects, selectedId = null) {
  const options = projects.map(item => `<option value="${item.id}" ${item.id === selectedId ? 'selected' : ''}>${escapeHtml(item.data.name)}</option>`).join('');
  const fields = `<label>专注项目<select name="project_id" required>${options}</select></label><label>计时方式<select id="timer-mode" name="timer_mode"><option value="countdown">倒计时</option><option value="stopwatch">正计时</option></select></label><label id="focus-duration">倒计时长度（分钟）<input name="minutes" type="number" min="1" max="240" value="25" required></label><button>确定并开始</button>`;
  modal('开始专注', fields, async form => {
    const timerMode = form.get('timer_mode');
    const plannedMinutes = timerMode === 'countdown' ? Number(form.get('minutes')) : 0;
    await api('/api/life/focus', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({project_id: Number(form.get('project_id')), started_at_ms: Date.now(), planned_minutes: plannedMinutes, timer_mode: timerMode, status: 'active'})
    });
    await focusTimerController.refresh();
    await loadSchedule();
  });
  const timerMode = $('#timer-mode');
  const durationField = $('#focus-duration');
  const syncDurationField = () => syncFocusDurationField(timerMode.value, durationField);
  timerMode.addEventListener('change', syncDurationField);
  syncDurationField();
}

async function loadSchedule() {
  const monthRange = rangeForMonth(scheduleState.month);
  try {
    const [events, tasks, focusRecords, checkinRecords, projects, insights] = await Promise.all([
      lifeRecords('calendar'),
      lifeRecords('tasks'),
      lifeRecords('focus'),
      lifeRecords('checkins'),
      lifeRecords('habits'),
      activityInsights(monthRange)
    ]);
    Object.assign(scheduleState, {events, tasks, focusRecords, checkinRecords, projects, insights});
    renderProjectActions(projects);
    renderScheduleCalendar();
    renderSelectedDay();
    const today = await activityInsights(rangeForDay(new Date()));
    $('#focus-today').textContent = `${Math.round(today.total_elapsed_seconds / 60)}m`;
  } catch (error) {
    $('#schedule-day-detail').innerHTML = `<p class="error">日程加载失败：${escapeHtml(error.message)}</p>`;
  }
}

document.querySelector('[data-action="new-event"]').onclick = () => modal('新建日历事项', `<label>标题<input name="title" required></label><label>开始<input type="datetime-local" name="start" value="${dateInput(Date.now() + 3600000)}" required></label><label>结束<input type="datetime-local" name="end" value="${dateInput(Date.now() + 7200000)}" required></label><button>保存</button>`, async form => {
  await api('/api/life/calendar', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({title: form.get('title'), starts_at_ms: new Date(form.get('start')).getTime(), ends_at_ms: new Date(form.get('end')).getTime(), timezone: Intl.DateTimeFormat().resolvedOptions().timeZone})
  });
  await loadSchedule();
});

document.querySelector('[data-action="new-task"]').onclick = () => modal('新建待办', `<label>标题<input name="title" required></label><label>截止时间<input type="datetime-local" name="due" value="${dateInput(Date.now() + 86400000)}" required></label><button>保存</button>`, async form => {
  await api('/api/life/tasks', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({title: form.get('title'), due_at_ms: new Date(form.get('due')).getTime(), completed: false})
  });
  await loadSchedule();
});

document.querySelector('[data-action="review-plan"]').onclick = async () => {
  const review = state.documents.filter(item => item.learning_status === 'needs_review');
  if (!review.length) {
    alert('当前没有待复习文档');
    return;
  }
  const start = Date.now() + 86400000;
  await api('/api/life/calendar', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({title: `复习：${review.slice(0, 3).map(item => item.filename).join('、')}`, starts_at_ms: start, ends_at_ms: start + 3600000, timezone: Intl.DateTimeFormat().resolvedOptions().timeZone})
  });
  alert('已把复习计划加入明天的日历');
  await loadSchedule();
};

function openProjectModal(projectType) {
  const iconOptions = '<option value="●">● 默认</option><option value="📚">📚 学习</option><option value="💻">💻 工作</option><option value="🏃">🏃 运动</option><option value="🧘">🧘 放松</option><option value="💪">💪 健身</option><option value="🎯">🎯 目标</option><option value="custom">自定义</option>';
  const fields = `<input type="hidden" name="project_type" value="${projectType}"><label>项目名<input name="name" maxlength="100" required></label><label>图标<select id="project-icon-preset" name="icon_preset">${iconOptions}</select></label><label id="project-custom-icon" hidden>自定义图标<input name="custom_icon" maxlength="8" placeholder="输入一个 Emoji 或短字符"></label><label>颜色<input name="color" type="color" value="#2F6F55"></label><label>备注<textarea name="note" maxlength="500" rows="3" placeholder="可选"></textarea></label><button>创建项目</button>`;
  modal(`添加${projectType === 'focus' ? '专注' : '打卡'}项目`, fields, async form => {
    const presetIcon = form.get('icon_preset');
    const projectIcon = presetIcon === 'custom' ? form.get('custom_icon').trim() : presetIcon;
    const result = await api('/api/life/habits', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({name: form.get('name').trim(), project_type: form.get('project_type'), icon: projectIcon, color: form.get('color'), note: form.get('note').trim(), archived: false})
    });
    if (result.duplicate) {
      alert('同名同类型的项目已经存在');
    }
    await loadSchedule();
  });
  $('#project-icon-preset').onchange = event => {
    const custom = event.target.value === 'custom';
    $('#project-custom-icon').hidden = !custom;
    $('#project-custom-icon input').required = custom;
  };
}

document.querySelector('[data-action="new-focus-project"]').onclick = () => openProjectModal('focus');
document.querySelector('[data-action="new-checkin-project"]').onclick = () => openProjectModal('checkin');

function selectMonth(offset) {
  scheduleState.month = new Date(scheduleState.month.getFullYear(), scheduleState.month.getMonth() + offset, 1);
  const today = new Date();
  const isCurrentMonth = scheduleState.month.getFullYear() === today.getFullYear() && scheduleState.month.getMonth() === today.getMonth();
  scheduleState.selectedDateKey = isCurrentMonth ? localDateKey(today) : localDateKey(scheduleState.month);
  loadSchedule();
}

$('#calendar-previous').onclick = () => selectMonth(-1);
$('#calendar-next').onclick = () => selectMonth(1);

async function loadLedger() {
  const month = localDateKey(new Date()).slice(0, 7);
  const [records, accounts, defaults, monthly, insights] = await Promise.all([lifeRecords('ledger'), lifeRecords('accounts'), lifeRecords('budget_settings'), lifeRecords('monthly_budgets'), api(`/api/life/finance/insights?month=${month}`)]);
  state.finance = {records, accounts, defaults, monthly, insights, month};
  const budgetLabel = insights.has_budget ? money(insights.remaining_budget_minor) : '未设置';
  const dailyLabel = insights.has_budget ? money(insights.remaining_daily_budget_minor) : '—';
  const defaultLabel = insights.default_budget_minor === null ? '尚未设置默认月预算' : `默认每月 ${money(insights.default_budget_minor)}`;
  $('#budget-overview').innerHTML = `<article class="card budget-primary"><span>本月剩余预算</span><strong>${budgetLabel}</strong><small>本月已支出 ${money(insights.month_expense_minor)}</small></article><article class="card budget-primary"><span>剩余日均预算</span><strong>${dailyLabel}</strong><small>按剩余 ${insights.remaining_days} 天计算</small></article><article class="budget-default"><span>${defaultLabel}</span></article>`;
  $('#ledger-summary').innerHTML = `<article class="card asset-metric"><span>总资产</span><strong>${money(insights.total_assets_minor)}</strong></article><article class="card asset-metric liability"><span>总负债</span><strong>${money(insights.total_liabilities_minor)}</strong></article><article class="card asset-metric"><span>净资产</span><strong>${money(insights.net_worth_minor)}</strong></article>`;
  $('#daily-expense').textContent = `日均支出 ${money(insights.average_daily_expense_minor)}`;
  renderExpenseChart(insights.categories, insights.month_expense_minor);
  $('#account-list').innerHTML = insights.accounts.filter(account => !account.archived).map(account => `<div class="account-row"><i style="--account-color:${account.color}"></i><span>${escapeHtml(account.name)}</span><strong class="${account.balance_minor < 0 ? 'liability' : ''}">${money(account.balance_minor)}</strong></div>`).join('') || '<p class="empty">还没有账户。</p>';
  $('#ledger-list').innerHTML = records.map(item => row(item, `${item.data.direction === 'expense' ? '支出' : '收入'} ¥${(item.data.amount_minor / 100).toFixed(2)} · ${item.data.category}`)).join('') || '<p class="empty">还没有账目</p>';
  bindDeletes(loadLedger);
}

function money(minor) {
  const sign = minor < 0 ? '-' : '';
  return `${sign}¥${Math.abs(minor / 100).toFixed(2)}`;
}

function renderExpenseChart(categories, total) {
  if (!categories.length || total <= 0) {
    $('#expense-chart').innerHTML = '<div class="donut empty-donut"></div><p class="empty">本月还没有支出。</p>';
    return;
  }
  let offset = 0;
  const segments = categories.map(item => {
    const percentage = item.amount_minor / total * 100;
    const color = categoryColor(item.category);
    const segment = `<circle cx="64" cy="64" r="52" pathLength="100" stroke="${color}" stroke-dasharray="${percentage} ${100 - percentage}" stroke-dashoffset="${-offset}"><title>${escapeHtml(item.category)}：${money(item.amount_minor)}</title></circle>`;
    offset += percentage;
    return segment;
  });
  const legend = categories.map(item => `<div class="legend-row" title="${escapeHtml(item.category)}：${money(item.amount_minor)}"><i style="--project-color:${categoryColor(item.category)}"></i><span>${escapeHtml(item.category)}</span><strong>${money(item.amount_minor)}</strong></div>`).join('');
  $('#expense-chart').innerHTML = `<svg class="expense-donut" viewBox="0 0 128 128" role="img" aria-label="本月支出分类图"><circle class="expense-track" cx="64" cy="64" r="52"></circle>${segments.join('')}</svg><div class="donut-legend">${legend}</div>`;
}

async function upsertLifeRecord(domain, existing, payload) {
  const path = existing ? `/api/life/${domain}/${existing.id}` : `/api/life/${domain}`;
  await api(path, {method: existing ? 'PATCH' : 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(payload)});
}

document.querySelector('[data-action="new-account"]').onclick = () => modal('添加资金账户', '<label>账户名称<input name="name" placeholder="例如：支付宝、招商银行" maxlength="100" required></label><label>当前余额（元，可为负数）<input name="balance" type="number" step="0.01" value="0" required></label><label>颜色<input name="color" type="color" value="#2F6F55"></label><button>保存账户</button>', async form => {
  await api('/api/life/accounts', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({name: form.get('name').trim(), currency: 'CNY', opening_balance_minor: Math.round(Number(form.get('balance')) * 100), color: form.get('color'), archived: false})});
  await loadLedger();
});

document.querySelector('[data-action="manage-budget"]').onclick = () => modal('设置月支出预算', '<label>预算金额（元）<input name="amount" type="number" min="0" step="0.01" required></label><label>生效方式<select name="scope"><option value="month">只修改本月</option><option value="default">设为每月默认，并应用到本月</option></select></label><button>保存预算</button>', async form => {
  const payload = {amount_minor: Math.round(Number(form.get('amount')) * 100), currency: 'CNY'};
  if (form.get('scope') === 'default') {
    await upsertLifeRecord('budget_settings', state.finance.defaults[0], payload);
  }
  const current = state.finance.monthly.find(item => item.data.month === state.finance.month);
  await upsertLifeRecord('monthly_budgets', current, {...payload, month: state.finance.month});
  await loadLedger();
});

document.querySelector('[data-action="new-ledger"]').onclick = async () => {
  const accounts = state.finance?.accounts || await lifeRecords('accounts');
  if (!accounts.length) {
    alert('请先添加一个资金账户。');
    return;
  }
  const options = accounts.filter(item => !item.data.archived).map(item => `<option value="${item.id}">${escapeHtml(item.data.name)}</option>`).join('');
  modal('记一笔', `<label>账户<select name="account_id" required>${options}</select></label><label>类型<select name="direction"><option value="expense">支出</option><option value="income">收入</option></select></label><label>金额（元）<input name="amount" type="number" min="0.01" step="0.01" required></label><label>分类<input name="category" required></label><button>保存</button>`, async form => {
    await api('/api/life/ledger', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({account_id: Number(form.get('account_id')), direction: form.get('direction'), amount_minor: Math.round(Number(form.get('amount')) * 100), currency: 'CNY', category: form.get('category').trim(), occurred_at_ms: Date.now()})});
    await loadLedger();
  });
};

async function loadHealth() {
  const [weights, sleeps, summary] = await Promise.all([lifeRecords('weight'), lifeRecords('sleep'), api('/api/life/summary')]);
  const latestWeight = summary.latest_weight_grams ? `${(summary.latest_weight_grams / 1000).toFixed(1)} kg` : '暂无';
  $('#weight-latest').textContent = latestWeight;
  const latestSleep = [...sleeps].sort((left, right) => right.data.started_at_ms - left.data.started_at_ms)[0];
  $('#sleep-latest').innerHTML = latestSleep ? `<span class="sleep-icon">☾</span><span><strong>${latestSleep.data.duration_minutes} 分钟</strong><small>${new Date(latestSleep.data.started_at_ms).toLocaleDateString('zh-CN')} · 质量 ${latestSleep.data.quality}/5</small></span>` : '<span class="empty">暂无睡眠记录</span>';
  renderLineChart('#sleep-chart', sleeps, item => item.data.duration_minutes / 60, item => item.data.started_at_ms, '小时');
  renderLineChart('#weight-chart', weights, item => item.data.grams / 1000, item => item.data.recorded_at_ms, 'kg');
  $('#weight-panel').innerHTML = `<h3>体重记录</h3>${weights.slice(0, 8).map(item => row(item, `${(item.data.grams / 1000).toFixed(1)} kg`)).join('') || '<p class="empty">暂无记录</p>'}`;
  $('#sleep-panel').innerHTML = `<h3>睡眠记录</h3><p class="empty">平均 ${summary.average_sleep_minutes || 0} 分钟</p>${sleeps.slice(0, 8).map(item => row(item, `${item.data.duration_minutes} 分钟 · 质量 ${item.data.quality}/5`)).join('') || '<p class="empty">暂无记录</p>'}`;
  bindDeletes(loadHealth);
}

function renderLineChart(target, records, valueOf, dateOf, unit) {
  const points = [...records].sort((left, right) => dateOf(left) - dateOf(right)).slice(-14);
  if (!points.length) {
    $(target).innerHTML = '<p class="empty">记录后将在这里形成趋势。</p>';
    return;
  }
  const values = points.map(valueOf);
  const minimum = Math.min(...values);
  const maximum = Math.max(...values);
  const spread = Math.max(maximum - minimum, 1);
  const coordinates = points.map((item, index) => ({x: 28 + index / Math.max(1, points.length - 1) * 544, y: 132 - (valueOf(item) - minimum) / spread * 96, value: valueOf(item), date: new Date(dateOf(item)).toLocaleDateString('zh-CN', {month: 'numeric', day: 'numeric'})}));
  const circles = coordinates.map(point => `<circle cx="${point.x}" cy="${point.y}" r="4"><title>${point.date} · ${point.value.toFixed(1)} ${unit}</title></circle>`).join('');
  $(target).innerHTML = `<svg viewBox="0 0 600 160" role="img" aria-label="最近趋势"><line x1="28" y1="132" x2="572" y2="132"></line><polyline points="${coordinates.map(point => `${point.x},${point.y}`).join(' ')}"></polyline>${circles}</svg><div class="chart-range"><span>${coordinates[0].date}</span><span>${coordinates.at(-1).date}</span></div>`;
}

document.querySelector('[data-action="record-weight"]').onclick = () => modal('记录体重', `<label>体重（公斤）<input name="weight" type="number" step="0.1" min="25" max="400" required></label><label>日期时间<input name="time" type="datetime-local" value="${dateInput(Date.now())}" required></label><button>保存</button>`, async form => {
  await api('/api/life/weight', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({grams: Math.round(Number(form.get('weight')) * 1000), recorded_at_ms: new Date(form.get('time')).getTime()})});
  await loadHealth();
});

document.querySelector('[data-action="record-sleep"]').onclick = () => modal('记录睡眠', `<label>入睡时间<input type="datetime-local" name="start" value="${dateInput(Date.now() - 8 * 3600000)}" required></label><label>起床时间<input type="datetime-local" name="end" value="${dateInput(Date.now())}" required></label><label>睡眠质量<select name="quality"><option value="5">很好</option><option value="4">不错</option><option value="3" selected>一般</option><option value="2">较差</option><option value="1">很差</option></select></label><button>保存</button>`, async form => {
  await api('/api/life/sleep', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({started_at_ms: new Date(form.get('start')).getTime(), ended_at_ms: new Date(form.get('end')).getTime(), quality: Number(form.get('quality'))})});
  await loadHealth();
});

$('#command-form').onsubmit = async event => {
  event.preventDefault();
  const panel = $('#command-result');
  try {
    const result = await api('/api/assistant/interpret', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({text: $('#command-text').value})
    });
    if (result.clarification) {
      panel.textContent = result.clarification;
      return;
    }
    panel.innerHTML = `<p><strong>${escapeHtml(result.preview.summary)}</strong></p><pre>${escapeHtml(JSON.stringify(result.preview.data, null, 2))}</pre><button id="confirm-command">确认写入</button>`;
    $('#confirm-command').onclick = async () => {
      await api('/api/assistant/confirm', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({proposal_token: result.proposal_token})
      });
      panel.textContent = '已确认并写入。';
    };
  } catch (error) {
    panel.innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`;
  }
};

window.loadLifeView = name => ({schedule: loadSchedule, ledger: loadLedger, health: loadHealth}[name]?.());

async function initializeLife() {
  scheduleState.selectedDateKey = localDateKey(new Date());
  const today = await activityInsights(rangeForDay(new Date()));
  $('#focus-today').textContent = `${Math.round(today.total_elapsed_seconds / 60)}m`;
}

focusTimerController = mountFocusTimer({
  request: requestJson,
  onChange: () => {
    initializeLife();
    if ($('#schedule-view').classList.contains('active')) {
      loadSchedule();
    }
  }
});

api('/api/me').then(async () => {
  await Promise.all([loadDocuments(), initializeLife()]);
}).catch(() => {});
