const FIVE_MINUTES_MS = 5 * 60 * 1000;

function breakSeconds(data, nowMs) {
  if (!Number.isInteger(data.break_started_at_ms) || !Number.isInteger(data.break_until_ms)) {
    return 0;
  }
  const effectiveEndMs = Math.min(nowMs, data.break_until_ms);
  return Math.max(0, Math.floor((effectiveEndMs - data.break_started_at_ms) / 1000));
}

function focusElapsedSeconds(data, nowMs) {
  const wallSeconds = Math.max(0, Math.floor((nowMs - data.started_at_ms) / 1000));
  const pausedSeconds = Math.max(0, data.paused_seconds || 0);
  return Math.max(0, wallSeconds - pausedSeconds - breakSeconds(data, nowMs));
}

function escapeHtml(value) {
  const entities = {'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'};
  return String(value).replace(/[&<>"]/g, character => entities[character]);
}

function formatClock(totalSeconds) {
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor(totalSeconds % 3600 / 60);
  const seconds = totalSeconds % 60;
  const shortClock = `${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
  return hours ? `${String(hours).padStart(2, '0')}:${shortClock}` : shortClock;
}

export function focusTimerPresentation(record, nowMs) {
  const data = record.data;
  const resting = Number.isInteger(data.break_until_ms) && nowMs < data.break_until_ms;
  if (resting) {
    const remainingSeconds = Math.max(0, Math.ceil((data.break_until_ms - nowMs) / 1000));
    return {phase: 'break', label: '休息中', seconds: remainingSeconds};
  }
  const elapsedSeconds = focusElapsedSeconds(data, nowMs);
  if ((data.timer_mode || 'countdown') === 'stopwatch') {
    return {phase: 'focus', label: '已专注', seconds: elapsedSeconds};
  }
  const remainingSeconds = Math.max(0, data.planned_minutes * 60 - elapsedSeconds);
  return {phase: 'focus', label: '剩余', seconds: remainingSeconds};
}

export function resumeFocus(record, nowMs) {
  const payload = {...record.data};
  payload.paused_seconds = Math.max(0, payload.paused_seconds || 0) + breakSeconds(payload, nowMs);
  delete payload.break_started_at_ms;
  delete payload.break_until_ms;
  return payload;
}

export function startFiveMinuteBreak(record, nowMs) {
  const payload = Number.isInteger(record.data.break_started_at_ms) ? resumeFocus(record, nowMs) : {...record.data};
  payload.paused_seconds = Math.max(0, payload.paused_seconds || 0);
  payload.break_started_at_ms = nowMs;
  payload.break_until_ms = nowMs + FIVE_MINUTES_MS;
  return payload;
}

export function completeFocus(record, nowMs) {
  return {...record.data, status: 'completed', ended_at_ms: nowMs};
}

export function syncFocusDurationField(mode, label) {
  const countdown = mode === 'countdown';
  const input = label.querySelector('input');
  label.hidden = !countdown;
  input.required = countdown;
  input.disabled = !countdown;
}

export function mountFocusTimer({request, onChange = () => {}}) {
  const host = document.createElement('aside');
  host.className = 'focus-timer-float';
  host.hidden = true;
  host.setAttribute('aria-live', 'polite');
  document.body.appendChild(host);

  let activeRecord = null;
  let projects = [];
  let updating = false;

  const patchRecord = async payload => {
    if (!activeRecord || updating) {
      return;
    }
    updating = true;
    render();
    try {
      const response = await request(`/api/life/focus/${activeRecord.id}`, {
        method: 'PATCH',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload)
      });
      activeRecord = response.record.data.status === 'active' ? response.record : null;
      onChange(response.record);
    } finally {
      updating = false;
      render();
    }
  };

  const render = () => {
    if (!activeRecord) {
      host.hidden = true;
      host.replaceChildren();
      return;
    }
    const project = projects.find(item => item.id === activeRecord.data.project_id);
    const presentation = focusTimerPresentation(activeRecord, Date.now());
    const projectName = project?.data.name || '未分类专注';
    const projectColor = project?.data.color || '#2F6F55';
    const primaryLabel = presentation.phase === 'break' ? '提前继续' : '休息 5 分钟';
    const disabled = updating ? 'disabled' : '';
    const heading =
      `<div class="focus-float-heading"><span style="--project-color:${projectColor}"></span><div>` +
      `<small>${escapeHtml(projectName)}</small>` +
      `<strong>${presentation.label} ${formatClock(presentation.seconds)}</strong></div></div>`;
    const actions =
      `<div class="focus-float-actions">` +
      `<button type="button" class="button-quiet" data-focus-primary ${disabled}>${primaryLabel}</button>` +
      `<button type="button" data-focus-complete ${disabled}>结束</button></div>`;
    host.hidden = false;
    host.innerHTML = heading + actions;
    host.querySelector('[data-focus-primary]').onclick = () => {
      const payload = presentation.phase === 'break'
        ? resumeFocus(activeRecord, Date.now())
        : startFiveMinuteBreak(activeRecord, Date.now());
      patchRecord(payload);
    };
    host.querySelector('[data-focus-complete]').onclick = () => patchRecord(completeFocus(activeRecord, Date.now()));
  };

  const refresh = async () => {
    const [focusResponse, projectResponse] = await Promise.all([
      request('/api/life/focus'),
      request('/api/life/habits')
    ]);
    activeRecord = focusResponse.records.find(item => item.data.status === 'active') || null;
    projects = projectResponse.records;
    render();
  };

  const interval = window.setInterval(() => {
    if (activeRecord && Number.isInteger(activeRecord.data.break_until_ms) &&
        Date.now() >= activeRecord.data.break_until_ms && !updating) {
      patchRecord(resumeFocus(activeRecord, Date.now()));
      return;
    }
    render();
  }, 1000);

  refresh().catch(error => console.error('focus timer unavailable', error));
  return {
    refresh,
    destroy() {
      window.clearInterval(interval);
      host.remove();
    }
  };
}
