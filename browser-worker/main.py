from __future__ import annotations

import os
import re
import time
import uuid
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

from fastapi import FastAPI
from pydantic import BaseModel
from playwright.async_api import async_playwright, Page


DATA_DIR = Path(os.getenv("BROWSER_DATA_DIR", "/data/browser"))
DATA_DIR.mkdir(parents=True, exist_ok=True)
SESSION_TTL_SECONDS = int(os.getenv("BROWSER_SESSION_TTL_SECONDS", "3600"))

app = FastAPI(title="Catch the Letter Browser Worker")


class InspectRequest(BaseModel):
    url: str
    session_id: str | None = None


class FillField(BaseModel):
    id: str
    selector: str | None = None
    value: str | None = None


class FillRequest(BaseModel):
    session_id: str
    fields: list[FillField]


class SubmitRequest(BaseModel):
    session_id: str


class CloseSessionRequest(BaseModel):
    session_id: str


class AuthCredentials(BaseModel):
    session_id: str
    username: str
    password: str


class TwoFactorCode(BaseModel):
    session_id: str
    code: str


sessions: dict[str, dict[str, Any]] = {}


async def close_session_id(session_id: str) -> bool:
    session = sessions.pop(session_id, None)
    if not session:
        return False
    for key in ("page", "context", "browser", "playwright"):
        obj = session.get(key)
        if not obj:
            continue
        try:
            if key == "playwright":
                await obj.stop()
            else:
                await obj.close()
        except Exception:
            pass
    return True


async def cleanup_expired_sessions() -> None:
    now = time.time()
    expired = [
        sid for sid, session in sessions.items()
        if now - float(session.get("last_used_at", session.get("created_at", now))) > SESSION_TTL_SECONDS
    ]
    for sid in expired:
        await close_session_id(sid)


def touch_session(session_id: str) -> dict[str, Any] | None:
    session = sessions.get(session_id)
    if session:
        session["last_used_at"] = time.time()
    return session


def form_type_for(url: str) -> str:
    parsed = urlparse(url)
    host = parsed.netloc.lower()
    path = parsed.path.lower()
    if host == "forms.gle" or (host == "docs.google.com" and "/forms" in path):
        return "google_forms"
    if host.endswith("forms.yandex.ru") or host == "forms.yandex.ru":
        return "yandex_forms"
    if host in {"forms.office.com", "forms.microsoft.com"}:
        return "microsoft_forms"
    if host == "portal.hse.ru" and path.startswith("/poll"):
        return "hse_portal_poll"
    if host.endswith("lms.hse.ru") or "smartlms" in host:
        return "hse_lms"
    return "generic_html_form"


async def detect_auth_required(page: Page) -> bool:
    url = page.url.lower()
    title = (await page.title()).lower()
    try:
        body = (await page.locator("body").inner_text(timeout=3000)).lower()
    except Exception:
        body = ""
    markers = [
        "login",
        "sign in",
        "войти",
        "авторизация",
        "пароль",
        "oauth",
        "smartpoint",
        "id.hse.ru",
        "lms.hse.ru/login",
    ]
    text = f"{url}\n{title}\n{body[:3000]}"
    return any(marker in text for marker in markers)


async def screenshot(page: Page, session_id: str, suffix: str) -> str:
    path = DATA_DIR / f"{session_id}-{suffix}.png"
    await page.screenshot(path=str(path), full_page=True)
    return str(path)


FIELD_SCRIPT = r"""
() => {
  function cssPath(el) {
    if (el.id) return '#' + CSS.escape(el.id);
    const parts = [];
    while (el && el.nodeType === Node.ELEMENT_NODE && parts.length < 5) {
      let part = el.nodeName.toLowerCase();
      if (el.name) {
        part += `[name="${CSS.escape(el.name)}"]`;
        parts.unshift(part);
        break;
      }
      const parent = el.parentElement;
      if (parent) {
        const same = Array.from(parent.children).filter(x => x.nodeName === el.nodeName);
        if (same.length > 1) part += `:nth-of-type(${same.indexOf(el) + 1})`;
      }
      parts.unshift(part);
      el = parent;
    }
    return parts.join(' > ');
  }
  function labelFor(node) {
    if (node.labels && node.labels.length) {
      return Array.from(node.labels).map(x => x.innerText.trim()).filter(Boolean).join(' ');
    }
    const aria = node.getAttribute('aria-label');
    if (aria) return aria;
    const placeholder = node.getAttribute('placeholder');
    if (placeholder) return placeholder;
    const parentLabel = node.closest('label');
    if (parentLabel) return parentLabel.innerText.trim();
    const block = node.closest('[role="listitem"], .freebirdFormviewerComponentsQuestionBaseRoot, div');
    if (block) return block.innerText.replace(/\s+/g, ' ').trim().slice(0, 300);
    return '';
  }
  function fieldType(node) {
    const tag = node.tagName.toLowerCase();
    const role = node.getAttribute('role');
    const type = (node.getAttribute('type') || '').toLowerCase();
    if (tag === 'textarea' || role === 'textbox') return tag === 'textarea' ? 'textarea' : 'text';
    if (tag === 'select') return 'select';
    if (role === 'radio' || type === 'radio') return 'radio';
    if (role === 'checkbox' || type === 'checkbox') return 'checkbox';
    if (type === 'email') return 'email';
    if (type === 'tel') return 'tel';
    if (tag === 'input') return type || 'text';
    return 'unknown';
  }
  const nodes = Array.from(document.querySelectorAll(
    'input:not([type="hidden"]), textarea, select, [role="textbox"], [role="radio"], [role="checkbox"]'
  ));
  const seen = new Set();
  const result = [];
  for (const node of nodes) {
    const selector = cssPath(node);
    if (!selector || seen.has(selector)) continue;
    seen.add(selector);
    const id = node.id || node.name || node.getAttribute('aria-label') || selector;
    const options = node.tagName.toLowerCase() === 'select'
      ? Array.from(node.options).map(o => o.text.trim()).filter(Boolean)
      : [];
    result.push({
      id,
      selector,
      label: labelFor(node),
      type: fieldType(node),
      required: Boolean(node.required || node.getAttribute('aria-required') === 'true'),
      options
    });
  }
  return result;
}
"""


@app.get("/health")
async def health() -> dict[str, Any]:
    return {"ok": True}


@app.get("/demo-form")
async def demo_form() -> Any:
    from fastapi.responses import HTMLResponse
    return HTMLResponse("""
<!doctype html><html><body>
<h1>Demo form</h1>
<form method="post" action="/demo-thanks">
  <label>ФИО <input name="full_name" required></label><br>
  <label>Email <input type="email" name="email" required></label><br>
  <label>Группа <input name="student_group"></label><br>
  <fieldset><legend>Оценка</legend>
    <label><input type="radio" name="rating" value="5">5</label>
    <label><input type="radio" name="rating" value="4">4</label>
    <label><input type="radio" name="rating" value="3">3</label>
  </fieldset>
  <label>Комментарий <textarea name="comment"></textarea></label><br>
  <button type="submit">Отправить</button>
</form>
</body></html>
""")


@app.get("/demo-auth-form")
async def demo_auth_form() -> Any:
    from fastapi.responses import HTMLResponse
    return HTMLResponse("""
<!doctype html><html><body>
<h1>Demo login</h1>
<div id="login">
  <label>Login <input id="login-input" autocomplete="username"></label>
  <label>Password <input id="password-input" type="password" autocomplete="current-password"></label>
  <button onclick="doLogin()">Войти</button>
</div>
<div id="twofa" style="display:none">
  <label>2FA code <input id="code-input" autocomplete="one-time-code" inputmode="numeric"></label>
  <button onclick="doCode()">Подтвердить</button>
</div>
<form id="form" style="display:none" method="post" action="/demo-thanks">
  <label>ФИО <input name="full_name" required></label><br>
  <label>Email <input type="email" name="email" required></label><br>
  <label>Группа <input name="student_group"></label><br>
  <button type="submit">Отправить</button>
</form>
<script>
function doLogin() {
  const u = document.getElementById('login-input').value;
  const p = document.getElementById('password-input').value;
  if (u === 'demo' && p === 'demo') {
    document.getElementById('login').style.display='none';
    document.getElementById('twofa').style.display='block';
  } else {
    document.body.dataset.authError = 'true';
  }
}
function doCode() {
  if (document.getElementById('code-input').value === '123456') {
    document.getElementById('twofa').style.display='none';
    document.getElementById('form').style.display='block';
  } else {
    document.body.dataset.authError = 'true';
  }
}
</script>
</body></html>
""")


@app.post("/inspect-form")
async def inspect_form(req: InspectRequest) -> dict[str, Any]:
    await cleanup_expired_sessions()
    sid = req.session_id or str(uuid.uuid4())
    try:
      playwright = await async_playwright().start()
      browser = await playwright.chromium.launch(headless=True)
      context = await browser.new_context()
      page = await context.new_page()
      await page.goto(req.url, wait_until="networkidle", timeout=60000)
      auth_required = await detect_auth_required(page)
      shot = await screenshot(page, sid, "inspect")
      fields: list[dict[str, Any]] = []
      if not auth_required:
          fields = await page.evaluate(FIELD_SCRIPT)
      sessions[sid] = {
          "playwright": playwright,
          "browser": browser,
          "context": context,
          "page": page,
          "created_at": time.time(),
          "last_used_at": time.time(),
      }
      return {
          "ok": True,
          "session_id": sid,
          "url": req.url,
          "final_url": page.url,
          "title": await page.title(),
          "form_type": form_type_for(page.url or req.url),
          "auth_required": auth_required,
          "fields": fields,
          "screenshot_path": shot,
          "error": "",
      }
    except Exception as exc:
      return {"ok": False, "session_id": sid, "url": req.url, "fields": [], "error": str(exc)}


async def fill_one(page: Page, field: FillField) -> None:
    selector = field.selector or field.id
    value = field.value or ""
    loc = page.locator(selector).first
    tag = ""
    typ = ""
    try:
        tag = (await loc.evaluate("el => el.tagName.toLowerCase()")).lower()
        typ = ((await loc.get_attribute("type")) or "").lower()
        role = ((await loc.get_attribute("role")) or "").lower()
    except Exception:
        role = ""
    if tag == "select":
        try:
            await loc.select_option(label=value)
        except Exception:
            await loc.select_option(value=value)
        return
    if typ in {"checkbox"} or role == "checkbox":
        if value.lower() in {"1", "true", "yes", "да", "on"}:
            await loc.check(force=True)
        else:
            await loc.uncheck(force=True)
        return
    if typ == "radio" or role == "radio":
        await loc.check(force=True)
        return
    await loc.fill(value)


@app.post("/fill-form")
async def fill_form(req: FillRequest) -> dict[str, Any]:
    await cleanup_expired_sessions()
    session = touch_session(req.session_id)
    if not session:
        return {"ok": False, "filled": [], "failed": [], "error": "session not found"}
    page: Page = session["page"]
    filled: list[str] = []
    failed: list[dict[str, str]] = []
    for field in req.fields:
        try:
            await fill_one(page, field)
            filled.append(field.id)
        except Exception as exc:
            failed.append({"id": field.id, "error": str(exc)})
    shot = await screenshot(page, req.session_id, "filled")
    return {"ok": len(failed) == 0, "filled": filled, "failed": failed, "screenshot_path": shot, "error": ""}


@app.post("/submit-form")
async def submit_form(req: SubmitRequest) -> dict[str, Any]:
    await cleanup_expired_sessions()
    session = touch_session(req.session_id)
    if not session:
        return {"ok": False, "submitted": False, "error": "session not found"}
    page: Page = session["page"]
    try:
        candidates = [
            'button[type="submit"]',
            'input[type="submit"]',
            'button:has-text("Отправить")',
            'button:has-text("Submit")',
            'button:has-text("Send")',
            'button:has-text("Готово")',
            'button:has-text("Завершить")',
            'button:has-text("Далее")',
            'button:has-text("Next")',
            'button:has-text("Continue")',
            'button:has-text("Продолжить")',
            'button:has-text("Сохранить")',
            '[role="button"]:has-text("Отправить")',
            '[role="button"]:has-text("Submit")',
            '[role="button"]:has-text("Далее")',
            '[role="button"]:has-text("Next")',
        ]
        clicked = False
        clicked_text = ""
        for selector in candidates:
            loc = page.locator(selector).first
            if await loc.count():
                try:
                    clicked_text = (await loc.inner_text()).strip().lower()
                except Exception:
                    clicked_text = selector.lower()
                await loc.click(timeout=5000)
                clicked = True
                break
        if not clicked:
            return {"ok": False, "submitted": False, "error": "submit button not found"}
        try:
            await page.wait_for_load_state("networkidle", timeout=15000)
        except Exception:
            pass
        shot = await screenshot(page, req.session_id, "submitted")
        if any(x in clicked_text for x in ["далее", "next", "continue", "продолжить"]):
            fields = await page.evaluate(FIELD_SCRIPT)
            return {
                "ok": True,
                "submitted": False,
                "status": "needs_next",
                "fields": fields,
                "final_url": page.url,
                "screenshot_path": shot,
                "error": "",
            }
        await close_session_id(req.session_id)
        return {"ok": True, "submitted": True, "final_url": page.url, "screenshot_path": shot, "error": ""}
    except Exception as exc:
        return {"ok": False, "submitted": False, "final_url": page.url, "error": str(exc)}


@app.post("/auth/credentials")
async def auth_credentials(req: AuthCredentials) -> dict[str, Any]:
    await cleanup_expired_sessions()
    session = touch_session(req.session_id)
    if not session:
        return {"ok": False, "status": "failed", "error": "session not found"}
    page: Page = session["page"]
    try:
        username_selectors = [
            'input[type="email"]',
            'input[autocomplete="username"]',
            'input[autocomplete="email"]',
            'input[name*="email" i]',
            'input[name*="login" i]',
            'input[name*="user" i]',
            'input[id*="email" i]',
            'input[id*="login" i]',
            'input[id*="user" i]',
        ]
        password_selectors = ['input[type="password"]', 'input[autocomplete="current-password"]']
        button_selectors = [
            'button:has-text("Войти")', 'button:has-text("Sign in")', 'button:has-text("Login")',
            'button:has-text("Log in")', 'button:has-text("Continue")', 'button:has-text("Продолжить")',
            'button:has-text("Далее")', 'button:has-text("Submit")', 'button:has-text("Подтвердить")',
            '[role="button"]:has-text("Войти")', '[role="button"]:has-text("Sign in")'
        ]
        user_filled = False
        for selector in username_selectors:
            loc = page.locator(selector).first
            if await loc.count():
                await loc.fill(req.username)
                user_filled = True
                break
        pass_filled = False
        for selector in password_selectors:
            loc = page.locator(selector).first
            if await loc.count():
                await loc.fill(req.password)
                pass_filled = True
                break
        if not user_filled or not pass_filled:
            return {"ok": False, "status": "failed", "error": "login or password field not found"}
        for selector in button_selectors:
            loc = page.locator(selector).first
            if await loc.count():
                await loc.click(timeout=5000)
                break
        try:
            await page.wait_for_load_state("networkidle", timeout=15000)
        except Exception:
            pass
        if await detect_2fa_required(page):
            return {"ok": True, "status": "waiting_2fa", "two_factor_hint": "Введите одноразовый код", "error": ""}
        if not await detect_auth_required(page):
            return {"ok": True, "status": "authenticated", "error": ""}
        return {"ok": False, "status": "failed", "error": "auth failed"}
    except Exception as exc:
        return {"ok": False, "status": "failed", "error": str(exc)}


@app.post("/auth/2fa")
async def auth_2fa(req: TwoFactorCode) -> dict[str, Any]:
    await cleanup_expired_sessions()
    session = touch_session(req.session_id)
    if not session:
        return {"ok": False, "status": "failed", "error": "session not found"}
    page: Page = session["page"]
    try:
        selectors = [
            'input[autocomplete="one-time-code"]',
            'input[name*="code" i]', 'input[id*="code" i]',
            'input[name*="otp" i]', 'input[id*="otp" i]',
            'input[type="tel"]', 'input[inputmode="numeric"]',
        ]
        filled = False
        for selector in selectors:
            loc = page.locator(selector).first
            if await loc.count():
                await loc.fill(req.code)
                filled = True
                break
        if not filled:
            return {"ok": False, "status": "failed", "error": "2FA field not found"}
        for selector in ['button:has-text("Подтвердить")', 'button:has-text("Continue")', 'button:has-text("Submit")', 'button:has-text("Далее")', '[role="button"]:has-text("Подтвердить")']:
            loc = page.locator(selector).first
            if await loc.count():
                await loc.click(timeout=5000)
                break
        try:
            await page.wait_for_load_state("networkidle", timeout=15000)
        except Exception:
            pass
        if not await detect_auth_required(page):
            return {"ok": True, "status": "authenticated", "error": ""}
        if await detect_2fa_required(page):
            return {"ok": True, "status": "waiting_2fa", "error": ""}
        return {"ok": False, "status": "failed", "error": "2FA failed"}
    except Exception as exc:
        return {"ok": False, "status": "failed", "error": str(exc)}


async def detect_2fa_required(page: Page) -> bool:
    try:
        text = (await page.locator("body").inner_text(timeout=3000)).lower()
    except Exception:
        text = ""
    markers = ["2fa", "verification", "one-time", "otp", "код", "подтвержд", "sms"]
    if any(marker in text for marker in markers):
        return True
    for selector in ['input[autocomplete="one-time-code"]', 'input[name*="code" i]', 'input[id*="otp" i]']:
        try:
            if await page.locator(selector).count():
                return True
        except Exception:
            pass
    return False


@app.post("/reinspect-form")
async def reinspect_form(req: SubmitRequest) -> dict[str, Any]:
    await cleanup_expired_sessions()
    session = touch_session(req.session_id)
    if not session:
        return {"ok": False, "session_id": req.session_id, "fields": [], "error": "session not found"}
    page: Page = session["page"]
    auth_required = await detect_auth_required(page)
    shot = await screenshot(page, req.session_id, "reinspect")
    fields: list[dict[str, Any]] = []
    if not auth_required:
        fields = await page.evaluate(FIELD_SCRIPT)
    return {
        "ok": True,
        "session_id": req.session_id,
        "url": page.url,
        "final_url": page.url,
        "title": await page.title(),
        "form_type": form_type_for(page.url),
        "auth_required": auth_required,
        "fields": fields,
        "screenshot_path": shot,
        "error": "",
    }


@app.post("/close-session")
async def close_session(req: CloseSessionRequest) -> dict[str, Any]:
    await cleanup_expired_sessions()
    closed = await close_session_id(req.session_id)
    return {"ok": True, "closed": closed, "error": ""}
