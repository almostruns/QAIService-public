export function buildChatRequestPayload({conversationId, message, requestId, webSearchEnabled,
                                         consentToken = null}) {
  const payload = {
    conversation_id: conversationId,
    message,
    request_id: requestId,
    web_search_enabled: Boolean(webSearchEnabled)
  };
  if (consentToken) {
    payload.web_search_consent_token = consentToken;
  }
  return payload;
}
