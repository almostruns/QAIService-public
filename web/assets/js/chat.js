import {requestJson} from './api.js';
import {mountFocusTimer} from './focus_timer.js';
import {createRequestId} from './request_id.js';
import {buildChatRequestPayload} from './chat_request.js';
import {followChatProgress, progressStagePresentation, resumeConversationProgress,
        syncFinalChatProgress} from './chat_progress.js';

const historyElement = document.getElementById('history');
const chatForm = document.getElementById('chat');
const messageInput = document.getElementById('message');
const sendButton = document.getElementById('send');
const webSearchToggle = document.getElementById('web-search-enabled');
const statusElement = document.getElementById('status');
const mobileModeSwitch = document.getElementById('mobile-mode-switch');
const desktopModeButtons = [...document.querySelectorAll('[data-chat-mode]')];
const modeTitle = document.getElementById('mode-title');
const modeEyebrow = document.getElementById('mode-eyebrow');
const conversationPanel = document.getElementById('conversation-panel');
const conversationList = document.getElementById('conversation-list');
const conversationToggle = document.getElementById('conversation-toggle');
const conversationClose = document.getElementById('conversation-close');
const newConversationButton = document.getElementById('new-conversation');
const webConsentDialog = document.getElementById('web-search-consent');
const webConsentQueries = document.getElementById('web-consent-queries');
const webConsentRisks = document.getElementById('web-consent-risks');

const modes = {
  private: {
    endpoint: '/api/assistant/chat',
    title: '让资料和记录变成可执行的答案。',
    eyebrow: 'PRIVATE ASSISTANT',
    empty: '可以询问知识库和生活记录，也可以让管家提出写入方案。'
  },
  general: {
    endpoint: '/api/chat',
    title: '和你的模型继续对话。',
    eyebrow: 'OPENAI-COMPATIBLE CHAT',
    empty: '普通对话和私人管家的上下文彼此隔离。'
  }
};

const state = {
  mode: 'private',
  activeConversationId: null,
  conversations: {private: [], general: []},
  pendingConversationIds: new Set(),
  handledRequestIds: new Set()
};

function activeMode() {
  return state.mode;
}

function emptyState(message) {
  const wrapper = document.createElement('div');
  wrapper.className = 'chat-empty';
  const mark = document.createElement('span');
  mark.textContent = '栖';
  const heading = document.createElement('h2');
  heading.textContent = '从一个具体问题开始。';
  const detail = document.createElement('p');
  detail.textContent = message;
  wrapper.append(mark, heading, detail);
  return wrapper;
}

function clearHistory() {
  historyElement.replaceChildren(emptyState(modes[activeMode()].empty));
}

function renderMessage(role, content, webSources = []) {
  const row = document.createElement('div');
  row.className = `message-row ${role === 'user' ? 'user' : 'assistant'}`;
  const bubble = document.createElement('div');
  bubble.className = 'message-bubble';
  bubble.textContent = content;
  row.appendChild(bubble);
  historyElement.appendChild(row);
  renderWebSources(webSources, row);
  historyElement.scrollTop = historyElement.scrollHeight;
  return row;
}

function renderProgress() {
  const row = document.createElement('div');
  row.className = 'message-row assistant progress-row';
  row.setAttribute('aria-label', '回答处理进度');
  const bubble = document.createElement('div');
  bubble.className = 'message-bubble progress-bubble';
  const details = document.createElement('details');
  details.className = 'progress-details';
  const summary = document.createElement('summary');
  summary.className = 'progress-summary';
  const heading = document.createElement('strong');
  heading.textContent = '准备处理请求';
  const detail = document.createElement('small');
  detail.textContent = '正在建立任务…';
  const track = document.createElement('span');
  track.className = 'progress-track';
  const bar = document.createElement('i');
  bar.className = 'progress-bar';
  track.appendChild(bar);
  summary.append(heading, detail, track);
  const stageList = document.createElement('ol');
  stageList.className = 'progress-stage-list';
  details.append(summary, stageList);
  bubble.appendChild(details);
  row.appendChild(bubble);
  historyElement.appendChild(row);
  historyElement.scrollTop = historyElement.scrollHeight;
  return row;
}

function updateProgress(row, progress) {
  const total = Math.max(1, progress.stage_count || 1);
  const current = Math.min(total, (progress.stage_index || 0) + 1);
  row.querySelector('strong').textContent = progress.stage_label || '正在处理';
  row.querySelector('small').textContent = progress.status === 'failed' ? '处理失败' : `第 ${current} / ${total} 步`;
  row.querySelector('.progress-bar').style.width = `${current / total * 100}%`;
  const stageList = row.querySelector('.progress-stage-list');
  stageList.replaceChildren();
  (progress.stages || []).map(progressStagePresentation).forEach(stage => {
    const item = document.createElement('li');
    item.className = `progress-stage ${stage.className}`;
    const symbol = document.createElement('span');
    symbol.className = 'progress-stage-symbol';
    symbol.setAttribute('aria-hidden', 'true');
    symbol.textContent = stage.symbol;
    const label = document.createElement('span');
    label.className = 'progress-stage-label';
    label.textContent = stage.label;
    const status = document.createElement('small');
    status.textContent = stage.statusLabel;
    item.append(symbol, label, status);
    stageList.appendChild(item);
  });
}

function pollProgress(requestId, row) {
  let stopped = false;
  followChatProgress({
    requestId,
    request: requestJson,
    onProgress: progress => updateProgress(row, progress),
    shouldContinue: () => !stopped
  });
  return () => {
    stopped = true;
  };
}

function renderConversationMessages(messages) {
  clearHistory();
  if (!messages.length) {
    return;
  }
  historyElement.replaceChildren();
  messages.forEach(item => renderMessage(item.role, item.content, item.web_sources));
}

function renderWebSources(sources, messageRow) {
  if (!sources?.length || !messageRow) {
    return;
  }
  const details = document.createElement('details');
  details.className = 'message-evidence web-sources';
  const summary = document.createElement('summary');
  summary.textContent = `查看 ${sources.length} 个网页来源`;
  details.appendChild(summary);
  sources.forEach(source => {
    const entry = document.createElement('div');
    entry.className = 'evidence-entry';
    const link = document.createElement('a');
    link.href = source.url;
    link.target = '_blank';
    link.rel = 'noopener noreferrer';
    link.textContent = `[${source.id}] ${source.title || source.site || source.url}`;
    const meta = document.createElement('small');
    meta.textContent = [source.site, source.published_at].filter(Boolean).join(' · ');
    entry.append(link, meta);
    details.appendChild(entry);
  });
  messageRow.appendChild(details);
}

const riskLabels = {
  phone: '手机号码',
  identity_card: '身份证号',
  bank_card: '银行卡号',
  email: '邮箱地址',
  exact_address: '精确门牌地址'
};

function requestWebSearchConsent(details) {
  webConsentQueries.replaceChildren();
  (details.outbound_queries || []).forEach(query => {
    const item = document.createElement('li');
    item.textContent = query;
    webConsentQueries.appendChild(item);
  });
  webConsentRisks.replaceChildren();
  (details.risk_types || []).forEach(risk => {
    const item = document.createElement('li');
    item.textContent = riskLabels[risk] || risk;
    webConsentRisks.appendChild(item);
  });
  webConsentDialog.showModal();
  return new Promise(resolve => {
    webConsentDialog.addEventListener('close', () => resolve(webConsentDialog.returnValue || 'cancel'), {once: true});
  });
}

async function refreshConversationMessages(mode, conversationId, baselineCount, waitForNewMessages) {
  for (let attempt = 0; attempt < 12; ++attempt) {
    const result = await requestJson(`/api/conversations/${conversationId}/messages`);
    if (activeMode() !== mode || state.activeConversationId !== conversationId) {
      return;
    }
    renderConversationMessages(result.messages);
    if (!waitForNewMessages || result.messages.length > baselineCount) {
      return;
    }
    await new Promise(resolve => window.setTimeout(resolve, 150));
  }
}

async function recoverConversationProgress(mode, conversationId, baselineCount) {
  if (state.pendingConversationIds.has(conversationId)) {
    return;
  }
  let progressRow = null;
  let recoveredRequestId = '';
  let ignoreRecoveredRequest = false;
  let observedRunning = false;
  const isCurrentConversation = () => activeMode() === mode && state.activeConversationId === conversationId;
  const finishRecovery = () => {
    state.pendingConversationIds.delete(conversationId);
    if (isCurrentConversation()) {
      sendButton.disabled = false;
      messageInput.disabled = false;
      webSearchToggle.disabled = false;
      renderConversationList();
    }
  };
  const result = await resumeConversationProgress({
    conversationId,
    request: requestJson,
    shouldContinue: () => isCurrentConversation() && !ignoreRecoveredRequest,
    onProgress: progress => {
      recoveredRequestId = progress.request_id || recoveredRequestId;
      if (recoveredRequestId && state.handledRequestIds.has(recoveredRequestId)) {
        ignoreRecoveredRequest = true;
        return;
      }
      if (!progressRow) {
        if (historyElement.querySelector('.chat-empty')) {
          historyElement.replaceChildren();
        }
        progressRow = renderProgress();
        state.pendingConversationIds.add(conversationId);
        sendButton.disabled = true;
        messageInput.disabled = true;
        webSearchToggle.disabled = true;
        renderConversationList();
      }
      observedRunning = observedRunning || progress.status === 'running';
      updateProgress(progressRow, progress);
      statusElement.textContent = progress.status === 'running' ? '后台任务仍在继续…' : '';
    },
    onCompleted: async requestId => {
      state.handledRequestIds.add(requestId);
      progressRow?.remove();
      await refreshConversationMessages(mode, conversationId, baselineCount, observedRunning);
      statusElement.textContent = '';
    },
    onFailed: requestId => {
      state.handledRequestIds.add(requestId);
      if (progressRow) {
        progressRow.classList.add('failed');
        progressRow.querySelector('strong').textContent = '后台回答失败，请重新发送';
        progressRow.querySelector('small').textContent = '任务已结束';
      }
      statusElement.textContent = '后台任务处理失败。';
    }
  });
  finishRecovery();
}

function renderEvidence(evidence, messageRow) {
  if (!evidence?.length || !messageRow) {
    return;
  }
  const details = document.createElement('details');
  details.className = 'message-evidence';
  const summary = document.createElement('summary');
  summary.textContent = `查看 ${evidence.length} 条参考资料`;
  details.appendChild(summary);
  evidence.forEach(item => {
    const entry = document.createElement('div');
    entry.className = 'evidence-entry';
    const link = document.createElement('a');
    link.href = `/api/documents/${item.document_id}/content`;
    link.target = '_blank';
    link.rel = 'noopener';
    link.textContent = `${item.filename}${item.page_number ? ` · 第 ${item.page_number} 页` : ''}`;
    const excerpt = document.createElement('small');
    excerpt.textContent = item.excerpt;
    entry.append(link, excerpt);
    details.appendChild(entry);
  });
  messageRow.appendChild(details);
}

function renderProposal(result, messageRow) {
  if (!result.requires_confirmation || !messageRow) {
    return;
  }
  const panel = document.createElement('div');
  panel.className = 'proposal-card';
  const summary = document.createElement('strong');
  summary.textContent = result.preview.summary;
  const detail = document.createElement('pre');
  detail.textContent = JSON.stringify(result.preview.data, null, 2);
  const confirmButton = document.createElement('button');
  confirmButton.type = 'button';
  confirmButton.textContent = '确认写入';
  confirmButton.addEventListener('click', async () => {
    confirmButton.disabled = true;
    try {
      await requestJson('/api/assistant/confirm', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({proposal_token: result.proposal_token})
      });
      confirmButton.textContent = '已写入';
    } catch (error) {
      confirmButton.disabled = false;
      confirmButton.textContent = '写入失败，重试';
    }
  });
  const cancelButton = document.createElement('button');
  cancelButton.type = 'button';
  cancelButton.className = 'button-link';
  cancelButton.textContent = '取消';
  cancelButton.addEventListener('click', () => panel.remove());
  panel.append(summary, detail, confirmButton, cancelButton);
  messageRow.appendChild(panel);
}

function renderConversationList() {
  const mode = activeMode();
  conversationList.replaceChildren();
  state.conversations[mode].forEach(conversation => {
    const item = document.createElement('div');
    item.className = 'conversation-item';
    if (conversation.id === state.activeConversationId) {
      item.classList.add('active');
    }
    const selectButton = document.createElement('button');
    selectButton.type = 'button';
    selectButton.className = 'conversation-select';
    selectButton.textContent = conversation.title;
    selectButton.title = conversation.title;
    selectButton.addEventListener('click', () => selectConversation(conversation.id));
    const deleteButton = document.createElement('button');
    deleteButton.type = 'button';
    deleteButton.className = 'conversation-delete';
    deleteButton.textContent = '×';
    deleteButton.title = '删除对话';
    deleteButton.disabled = state.pendingConversationIds.has(conversation.id);
    deleteButton.addEventListener('click', () => deleteConversation(conversation.id));
    item.append(selectButton, deleteButton);
    conversationList.appendChild(item);
  });
}

async function createConversation(selectCreated = true) {
  const mode = activeMode();
  const result = await requestJson('/api/conversations', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({mode})
  });
  state.conversations[mode].unshift(result.conversation);
  renderConversationList();
  if (selectCreated) {
    await selectConversation(result.conversation.id);
  }
  return result.conversation;
}

async function loadConversations(mode, preferredId = null) {
  statusElement.textContent = '正在加载对话…';
  try {
    const result = await requestJson(`/api/conversations?mode=${mode}`);
    state.conversations[mode] = result.conversations;
    if (!state.conversations[mode].length) {
      await createConversation(false);
    }
    const preferredExists = state.conversations[mode].some(item => item.id === preferredId);
    const nextId = preferredExists ? preferredId : state.conversations[mode][0].id;
    await selectConversation(nextId);
  } catch (error) {
    state.activeConversationId = null;
    clearHistory();
    renderConversationList();
    statusElement.textContent = error.code === 'network_unavailable' ? '无法连接聊天服务。' : '对话列表加载失败。';
  }
}

async function selectConversation(conversationId) {
  const requestedMode = activeMode();
  state.activeConversationId = conversationId;
  clearHistory();
  renderConversationList();
  statusElement.textContent = '正在加载这段对话…';
  try {
    const result = await requestJson(`/api/conversations/${conversationId}/messages`);
    if (activeMode() !== requestedMode || state.activeConversationId !== conversationId) {
      return;
    }
    renderConversationMessages(result.messages);
    statusElement.textContent = '';
    if (window.innerWidth <= 900) {
      closeConversationPanel();
    }
    recoverConversationProgress(requestedMode, conversationId, result.messages.length);
  } catch (error) {
    statusElement.textContent = '这段对话加载失败。';
  }
}

async function deleteConversation(conversationId) {
  if (state.pendingConversationIds.has(conversationId)) {
    return;
  }
  if (!window.confirm('删除后无法恢复这段对话，确定继续吗？')) {
    return;
  }
  const mode = activeMode();
  try {
    await requestJson(`/api/conversations/${conversationId}`, {method: 'DELETE'});
    state.conversations[mode] = state.conversations[mode].filter(item => item.id !== conversationId);
    if (!state.conversations[mode].length) {
      await createConversation();
      return;
    }
    const nextId = state.activeConversationId === conversationId
      ? state.conversations[mode][0].id
      : state.activeConversationId;
    await selectConversation(nextId);
  } catch (error) {
    statusElement.textContent = error.code === 'conversation_busy'
      ? '这段对话正在生成回答，暂时不能删除。'
      : '删除对话失败。';
  }
}

function updateModePresentation() {
  const selected = modes[activeMode()];
  modeTitle.textContent = selected.title;
  modeEyebrow.textContent = selected.eyebrow;
}

function syncModeControls() {
  mobileModeSwitch.value = activeMode();
  desktopModeButtons.forEach(button => {
    const selected = button.dataset.chatMode === activeMode();
    button.classList.toggle('active', selected);
    button.setAttribute('aria-pressed', String(selected));
  });
}

async function changeMode(mode) {
  if (!modes[mode] || mode === activeMode()) {
    syncModeControls();
    return;
  }
  state.mode = mode;
  syncModeControls();
  updateModePresentation();
  state.activeConversationId = null;
  renderConversationList();
  await loadConversations(mode);
}

function openConversationPanel() {
  conversationPanel.classList.add('open');
  conversationToggle.setAttribute('aria-expanded', 'true');
}

function closeConversationPanel() {
  conversationPanel.classList.remove('open');
  conversationToggle.setAttribute('aria-expanded', 'false');
}

desktopModeButtons.forEach(button => {
  button.addEventListener('click', () => changeMode(button.dataset.chatMode));
});
mobileModeSwitch.addEventListener('change', () => changeMode(mobileModeSwitch.value));

newConversationButton.addEventListener('click', async () => {
  newConversationButton.disabled = true;
  try {
    await createConversation();
  } catch (error) {
    statusElement.textContent = '新建对话失败。';
  } finally {
    newConversationButton.disabled = false;
  }
});

conversationToggle.addEventListener('click', openConversationPanel);
conversationClose.addEventListener('click', closeConversationPanel);

chatForm.addEventListener('submit', async event => {
  event.preventDefault();
  const prompt = messageInput.value.trim();
  const conversationId = state.activeConversationId;
  const mode = activeMode();
  const webSearchEnabled = webSearchToggle.checked;
  if (!prompt || !conversationId || state.pendingConversationIds.has(conversationId)) {
    return;
  }
  if (historyElement.querySelector('.chat-empty')) {
    historyElement.replaceChildren();
  }
  const userRow = renderMessage('user', prompt);
  const progressRow = renderProgress();
  messageInput.value = '';
  state.pendingConversationIds.add(conversationId);
  sendButton.disabled = true;
  messageInput.disabled = true;
  webSearchToggle.disabled = true;
  renderConversationList();
  statusElement.textContent = '正在处理请求…';
  let stopProgress = () => {};
  let activeRequestId = '';
  let consentToken = null;
  let restoreInput = true;
  try {
    let result;
    while (!result) {
      const requestId = createRequestId();
      activeRequestId = requestId;
      const payload = buildChatRequestPayload({
        conversationId,
        message: prompt,
        requestId,
        webSearchEnabled,
        consentToken
      });
      const responsePromise = requestJson(modes[mode].endpoint, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload)
      });
      stopProgress = pollProgress(requestId, progressRow);
      try {
        result = await responsePromise;
      } catch (error) {
        stopProgress();
        if (error.code !== 'web_search_consent_required') {
          throw error;
        }
        const decision = await requestWebSearchConsent(error.details);
        if (decision === 'allow') {
          consentToken = error.details.consent_token;
          progressRow.classList.remove('failed');
          progressRow.querySelector('strong').textContent = '按你的选择继续搜索';
          progressRow.querySelector('small').textContent = '授权仅本次有效';
          continue;
        }
        if (decision === 'modify') {
          messageInput.value = prompt;
        } else {
          restoreInput = false;
        }
        userRow.remove();
        progressRow.remove();
        const cancelled = new Error('web_search_cancelled');
        cancelled.code = 'web_search_cancelled';
        throw cancelled;
      }
    }
    if (activeMode() === mode && state.activeConversationId === conversationId) {
      progressRow.remove();
      const assistantRow = renderMessage('assistant', result.message, result.web_sources);
      renderEvidence(result.evidence, assistantRow);
      renderProposal(result, assistantRow);
      statusElement.textContent = '';
    }
    const list = await requestJson(`/api/conversations?mode=${mode}`);
    state.conversations[mode] = list.conversations;
  } catch (error) {
    if (activeMode() === mode && state.activeConversationId === conversationId) {
      if (activeRequestId && progressRow.isConnected) {
        await syncFinalChatProgress({
          requestId: activeRequestId,
          request: requestJson,
          onProgress: progress => updateProgress(progressRow, progress)
        });
      }
      if (restoreInput && error.code !== 'web_search_cancelled') {
        messageInput.value = prompt;
      }
      progressRow.classList.add('failed');
      progressRow.querySelector('strong').textContent = error.code === 'web_search_cancelled' ? '已取消联网搜索' : '回答失败，请稍后重试';
      progressRow.querySelector('small').textContent = messageInput.value ? '输入内容已恢复' : '未向搜索服务发送内容';
      const errors = {
        model_timeout: '模型响应超时，可以再次发送。',
        model_unavailable: '模型暂时不可用。',
        model_upstream_error: '模型服务返回错误。',
        assistant_invalid_response: '模型没有按管家协议返回，请换一种说法。',
        private_context_unavailable: '私人资料暂时无法读取。',
        chat_busy: '当前对话正在处理上一条消息。',
        web_search_not_configured: '这个问题需要联网，但尚未配置百度搜索 Key。',
        web_search_quota_exhausted: '百度搜索额度已用完，无法核实这条实时信息。',
        web_search_rate_limited: '搜索请求过于频繁，请稍后再试。',
        web_search_timeout: '网页搜索超时，请稍后再试。',
        web_search_unauthorized: '百度搜索 Key 无效或无权限。',
        web_search_planner_unavailable: '暂时无法判断是否需要联网。',
        web_search_consent_invalid_or_expired: '本次授权已失效，请重新确认。',
        web_search_cancelled: '已取消本次联网搜索。'
      };
      statusElement.textContent = errors[error.code] || '发送失败，输入内容已保留。';
    }
  } finally {
    stopProgress();
    state.pendingConversationIds.delete(conversationId);
    sendButton.disabled = false;
    messageInput.disabled = false;
    webSearchToggle.disabled = false;
    renderConversationList();
    messageInput.focus();
  }
});

messageInput.addEventListener('keydown', event => {
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault();
    chatForm.requestSubmit();
  }
});

updateModePresentation();
syncModeControls();
loadConversations('private');
mountFocusTimer({request: requestJson});
