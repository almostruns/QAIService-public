export class ApiError extends Error {
  constructor(code, status, details = {}) {
    super(code);
    this.code = code;
    this.status = status;
    this.details = details;
  }
}

export async function requestJson(path, options = {}, redirectOnUnauthorized = true) {
  let response;
  try {
    response = await fetch(path, options);
  } catch (error) {
    throw new ApiError('network_unavailable', 0);
  }

  if (response.status === 401 && redirectOnUnauthorized) {
    location.assign('/login');
    throw new ApiError('authentication_required', 401);
  }
  if (response.status === 204) {
    if (!response.ok) {
      throw new ApiError('request_failed', response.status);
    }
    return null;
  }

  const contentType = response.headers.get('content-type') || '';
  let body;
  if (contentType.includes('application/json')) {
    body = await response.json();
  } else {
    body = {error: await response.text()};
  }
  if (!response.ok) {
    throw new ApiError(body.error || 'request_failed', response.status, body);
  }
  return body;
}
