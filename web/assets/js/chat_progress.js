const defaultDelay = () => new Promise(resolve => window.setTimeout(resolve, 120));

const stagePresentations = {
  completed: {statusLabel: '已完成', symbol: '✓'},
  running: {statusLabel: '进行中', symbol: '●'},
  pending: {statusLabel: '未开始', symbol: '○'},
  skipped: {statusLabel: '本次无需执行', symbol: '–'},
  failed: {statusLabel: '失败', symbol: '!'}
};

export function progressStagePresentation(stage) {
  const status = Object.hasOwn(stagePresentations, stage?.status) ? stage.status : 'pending';
  return {...stage, status, ...stagePresentations[status], className: `progress-stage-${status}`};
}

export async function followChatProgress({requestId, initialProgress, request, onProgress,
                                          onCompleted = () => {}, onFailed = () => {},
                                          delay = defaultDelay, shouldContinue = () => true}) {
  let progress = initialProgress;
  while (shouldContinue()) {
    if (!progress) {
      try {
        progress = await request(`/api/chat/progress/${encodeURIComponent(requestId)}`);
      } catch (error) {
        if (error.code !== 'chat_progress_not_found') {
          return 'unavailable';
        }
        await delay();
        continue;
      }
    }
    onProgress(progress);
    if (progress.status === 'completed') {
      await onCompleted(requestId);
      return 'completed';
    }
    if (progress.status === 'failed') {
      onFailed(requestId);
      return 'failed';
    }
    progress = null;
    await delay();
  }
  return 'stopped';
}

export async function resumeConversationProgress({conversationId, request, onProgress,
                                                  onCompleted = () => {}, onFailed = () => {},
                                                  delay = defaultDelay, shouldContinue = () => true}) {
  let initialProgress;
  try {
    initialProgress = await request(`/api/chat/progress/conversation/${encodeURIComponent(conversationId)}`);
  } catch (error) {
    return error.code === 'chat_progress_not_found' ? 'not_found' : 'unavailable';
  }
  initialProgress.request_id = initialProgress.request_id || '';
  return followChatProgress({
    requestId: initialProgress.request_id,
    initialProgress,
    request,
    onProgress,
    onCompleted,
    onFailed,
    delay,
    shouldContinue
  });
}

export async function syncFinalChatProgress({requestId, request, onProgress}) {
  try {
    const progress = await request(`/api/chat/progress/${encodeURIComponent(requestId)}`);
    onProgress(progress);
    return progress.status;
  } catch (error) {
    return 'unavailable';
  }
}
