import {requestJson} from './api.js';

const loginForm = document.getElementById('login-form');
const registerForm = document.getElementById('register-form');
const form = loginForm || registerForm;
const message = document.getElementById('message');
const submit = document.getElementById('submit');

const errorMessages = {
  invalid_registration: '用户名或密码不符合页面中的规则。',
  username_exists: '这个用户名已经存在，请换一个。',
  registration_disabled: '当前服务已经关闭新用户注册。',
  invalid_login: '请输入符合规则的用户名和密码。',
  invalid_credentials: '用户名或密码不正确。',
  database_busy: '服务繁忙，请稍后再试。',
  database_unavailable: '账户服务暂时不可用。',
  network_unavailable: '无法连接服务，请检查网络。'
};

form?.addEventListener('submit', async event => {
  event.preventDefault();
  const mode = loginForm ? 'login' : 'register';
  const data = Object.fromEntries(new FormData(form));
  submit.disabled = true;
  submit.textContent = mode === 'login' ? '登录中…' : '创建中…';
  message.textContent = '';
  try {
    await requestJson(`/api/${mode}`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(data)
    }, false);
    location.assign(mode === 'login' ? '/app' : '/login');
  } catch (error) {
    message.textContent = errorMessages[error.code] || '操作失败，请稍后重试。';
  } finally {
    submit.disabled = false;
    submit.textContent = mode === 'login' ? '登录' : '创建账户';
  }
});
