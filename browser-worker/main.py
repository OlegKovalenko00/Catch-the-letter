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
    debug: bool = False


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


class ClickRequest(BaseModel):
    x: int
    y: int


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


# Chrome 124 UA used for all contexts so Yandex SmartCaptcha sees a plausible browser.
_CHROME_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36"
)

# Init script injected into every page to hide Playwright/automation markers.
STEALTH_INIT_SCRIPT = """
(function () {
  // Hide webdriver flag
  Object.defineProperty(navigator, 'webdriver', {get: () => undefined, configurable: true});

  // Realistic plugin list
  const _plugins = [
    {name: 'Chrome PDF Plugin', filename: 'internal-pdf-viewer', description: 'Portable Document Format'},
    {name: 'Chrome PDF Viewer', filename: 'mhjfbmdgcfjbbpaeojofohoefgiehjai', description: ''},
    {name: 'Native Client', filename: 'internal-nacl-plugin', description: ''},
  ];
  _plugins.item = i => _plugins[i];
  _plugins.namedItem = n => _plugins.find(p => p.name === n) || null;
  _plugins.refresh = () => {};
  Object.defineProperty(navigator, 'plugins', {get: () => _plugins});

  // Languages matching Russian locale
  Object.defineProperty(navigator, 'languages', {get: () => ['ru-RU', 'ru', 'en-US', 'en']});

  // chrome runtime stub
  if (!window.chrome) {
    window.chrome = {runtime: {}, loadTimes: () => ({}), csi: () => ({}), app: {}};
  }

  // Remove Playwright globals
  try { delete window.__playwright; } catch(_) {}
  try { delete window.__pwInitScripts; } catch(_) {}
  try { delete window.playwright; } catch(_) {}
})();
"""


async def create_stealth_context(playwright):
    """Launch Chromium with stealth settings that reduce bot-detection signals."""
    browser = await playwright.chromium.launch(
        headless=True,
        args=[
            "--disable-blink-features=AutomationControlled",
            "--disable-dev-shm-usage",
            "--no-sandbox",
            "--disable-setuid-sandbox",
            "--disable-gpu",
        ],
    )
    context = await browser.new_context(
        user_agent=_CHROME_UA,
        locale="ru-RU",
        timezone_id="Europe/Moscow",
        viewport={"width": 1366, "height": 768},
        extra_http_headers={"Accept-Language": "ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7"},
    )
    await context.add_init_script(STEALTH_INIT_SCRIPT)
    return browser, context


async def wait_for_yandex_forms(page: Page) -> None:
    """Extra wait for Yandex Forms React rendering after domcontentloaded."""
    # Primary: Yandex Forms always renders inputs with name="answer_*" — most reliable
    try:
        await page.wait_for_selector('input[name^="answer_"], textarea[name^="answer_"]', timeout=12000)
        await page.wait_for_timeout(800)  # let React finish rendering question titles
        return
    except Exception:
        pass
    yandex_selectors = [
        ".b-form-question",
        "[data-form-id]",
        "form .field",
        "form input:not([type='hidden'])",
        "form textarea",
        ".form__container",
        "[class*='form-question']",
    ]
    for sel in yandex_selectors:
        try:
            await page.wait_for_selector(sel, timeout=8000)
            await page.wait_for_timeout(500)
            return
        except Exception:
            pass
    # Fallback: wait for network to settle a bit more
    try:
        await page.wait_for_load_state("networkidle", timeout=8000)
    except Exception:
        pass


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


def infer_virtual_fields_from_text(text: str) -> list[dict[str, Any]]:
    fields: list[dict[str, Any]] = []
    seen: set[str] = set()
    lines = [re.sub(r"\s+", " ", x).strip() for x in text.splitlines()]
    lines = [x for x in lines if x and len(x) <= 180]
    option_pattern = re.compile(r"(^|\s)(5|4|3|2|1)(\s|$)")
    for idx, line in enumerate(lines[:80]):
        lower = line.lower()
        if any(skip in lower for skip in ["отправить", "далее", "submit", "continue", "назад"]):
            continue
        field_type = ""
        options: list[dict[str, str]] = []
        if any(x in lower for x in ["email", "e-mail", "почта"]):
            field_type = "email"
        elif any(x in lower for x in ["телефон", "phone", "mobile"]):
            field_type = "tel"
        elif any(x in lower for x in ["комментар", "отзыв", "мнение", "пожел"]):
            field_type = "textarea"
        elif any(x in lower for x in ["оцен", "rating", "rate"]) or option_pattern.search(line):
            field_type = "radio_group"
            found = re.findall(r"\b[1-5]\b", line)
            if not found and idx + 1 < len(lines):
                found = re.findall(r"\b[1-5]\b", lines[idx + 1])
            options = [{"label": x, "value": x, "selector": "", "id": ""} for x in dict.fromkeys(found or ["5", "4", "3"])]
        elif any(x in lower for x in ["интерес", "выберите несколько", "checkbox"]):
            field_type = "checkbox_group"
        elif any(x in lower for x in ["фио", "имя", "фамилия", "группа", "факультет", "программа", "курс", "кампус"]):
            field_type = "text"
        if not field_type:
            continue
        key = re.sub(r"[^a-zа-я0-9]+", "_", lower).strip("_")[:40] or f"virtual_{idx}"
        if key in seen:
            continue
        seen.add(key)
        fields.append({
            "id": f"virtual_{key}",
            "selector": "",
            "label": line,
            "normalized_label": lower,
            "type": field_type,
            "required": False,
            "options": options,
            "value": "",
            "values": [],
            "semantic_key": "unknown",
            "mapped_profile_key": "",
            "confidence": 0.0,
            "source": "debug",
            "reason": "inferred from visible text because DOM extraction found no controls",
            "requires_user_input": True,
            "can_auto_fill": False,
            "unsupported_reason": "LLM/text-inferred field has no reliable DOM selector",
            "validation_error": "",
            "question_block_text": line,
            "nearby_text": line,
        })
        if len(fields) >= 12:
            break
    return fields


async def detect_auth_required(page: Page) -> bool:
    url = page.url.lower()
    title = (await page.title()).lower()
    try:
        body = (await page.locator("body").inner_text(timeout=3000)).lower()
    except Exception:
        body = ""
    try:
        if await page.locator('input[type="password"]:visible').count():
            return True
    except Exception:
        pass
    try:
        visible_form_fields = await page.locator('input:not([type="hidden"]):visible, textarea:visible, select:visible').count()
    except Exception:
        visible_form_fields = 0
    if visible_form_fields > 1:
        return False
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


async def detect_captcha_required(page: Page) -> bool:
    try:
        text = (await page.locator("body").inner_text(timeout=3000)).lower()
    except Exception:
        text = ""
    markers = ["smartcaptcha", "captcha", "recaptcha", "hcaptcha", "i am not a robot", "я не робот"]
    if any(marker in text for marker in markers):
        return True
    selectors = [
        ".SmartCaptcha",
        "[class*='captcha' i]",
        "[id*='captcha' i]",
        "[data-testid*='captcha' i]",
        "iframe[src*='captcha' i]",
        "iframe[src*='recaptcha' i]",
        "iframe[src*='hcaptcha' i]",
    ]
    for selector in selectors:
        try:
            if await page.locator(selector).count():
                return True
        except Exception:
            pass
    return False


async def screenshot(page: Page, session_id: str, suffix: str) -> str:
    path = DATA_DIR / f"{session_id}-{suffix}.png"
    await page.screenshot(path=str(path), full_page=True)
    return str(path)


FIELD_SCRIPT = r"""
() => {
  // Detect invisible/blank Unicode: U+3164 Hangul Filler, zero-width spaces, NBSP, BOM, etc.
  function isBlankText(text) {
    if (!text) return true;
    // Strip Hangul Filler (U+3164), zero-width spaces, NBSP, BOM, and whitespace
    return text.replace(/[ㅤ​‌‍\u200E\u200F ﻿⁠᠎\s]/g, '').length === 0;
  }
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
  function compact(text) {
    // Strip invisible Unicode chars before whitespace normalization
    return (text || '').replace(/[ㅤ​‌‍‎‏﻿ ⁠᠎͏]/g, '').replace(/\s+/g, ' ').trim();
  }
  function normalized(text) {
    return compact(text).toLowerCase();
  }
  // Walk DOM ancestors to find Yandex Forms question title text (sits in a sibling div above input).
  // Yandex Forms does NOT use <label for="..."> — question text is in a preceding sibling/ancestor div.
  function yandexAncestorLabel(node) {
    let cur = node.parentElement;
    for (let depth = 0; depth < 9 && cur && cur !== document.body; depth++) {
      const parent = cur.parentElement;
      if (parent) {
        const siblings = Array.from(parent.children);
        const idx = siblings.indexOf(cur);
        // Check up to 3 preceding siblings for question text
        for (let i = Math.max(0, idx - 3); i < idx; i++) {
          const sib = siblings[i];
          const tag = sib.tagName;
          if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') continue;
          const txt = compact(sib.innerText || sib.textContent || '');
          if (!isBlankText(txt) && txt.length > 2 && txt.length < 600) return txt;
        }
      }
      // Look for title-class children of current ancestor that don't contain node
      const titleEls = cur.querySelectorAll('[class*="title" i], [class*="label" i], [class*="caption" i], [class*="question" i], [class*="heading" i], legend');
      for (const t of titleEls) {
        if (t === node || t.contains(node) || node.contains(t)) continue;
        const txt = compact(t.innerText || t.textContent || '');
        if (!isBlankText(txt) && txt.length > 2 && txt.length < 600) return txt;
      }
      cur = cur.parentElement;
    }
    return '';
  }
  function nearestQuestion(node) {
    const rawType = (node.getAttribute('type') || '').toLowerCase();
    const role = (node.getAttribute('role') || '').toLowerCase();
    if (rawType === 'radio' || rawType === 'checkbox' || role === 'radio' || role === 'checkbox') {
      return node.closest('fieldset, [role="radiogroup"], [role="group"], [role="listitem"], .freebirdFormviewerComponentsQuestionBaseRoot, div') || node.parentElement || node;
    }
    const label = node.closest('label');
    if (label) return label;
    return node.closest('fieldset, [role="radiogroup"], [role="group"], [role="listitem"], .freebirdFormviewerComponentsQuestionBaseRoot, form, div') || node.parentElement || node;
  }
  function containerLabel(container, node) {
    const legend = container.querySelector('legend');
    if (legend) {
      const t = compact(legend.innerText);
      if (!isBlankText(t)) return t;
    }
    const aria = container.getAttribute('aria-label');
    if (aria && !isBlankText(aria)) return aria;
    // Yandex Forms: walk ancestors for question title
    const name = node.getAttribute('name') || '';
    if (name.startsWith('answer_')) {
      const yLabel = yandexAncestorLabel(node);
      if (!isBlankText(yLabel)) return yLabel;
    }
    const text = compact(container.innerText);
    const option = optionLabel(node);
    if (option && text.endsWith(option)) return compact(text.slice(0, text.length - option.length));
    return isBlankText(text) ? '' : text.slice(0, 300);
  }
  function optionLabel(node) {
    if (node.labels && node.labels.length) {
      const t = compact(Array.from(node.labels).map(x => x.innerText).join(' '));
      if (!isBlankText(t)) return t;
    }
    const aria = node.getAttribute('aria-label');
    if (aria && !isBlankText(aria)) return aria;
    const parentLabel = node.closest('label');
    if (parentLabel) {
      const t = compact(parentLabel.innerText);
      if (!isBlankText(t)) return t;
    }
    const parent = node.parentElement;
    const t = parent ? compact(parent.innerText) : '';
    return (!isBlankText(t)) ? t : (node.value || '');
  }
  function labelFor(node) {
    // 1. <label for="id"> association
    if (node.labels && node.labels.length) {
      const t = Array.from(node.labels).map(x => x.innerText.trim()).filter(x => !isBlankText(x)).join(' ');
      if (!isBlankText(t)) return t;
    }
    // 2. aria-label
    const aria = node.getAttribute('aria-label');
    if (aria && !isBlankText(aria)) return aria;
    // 3. placeholder (skip if invisible Unicode like U+3164 from Yandex Forms)
    const placeholder = node.getAttribute('placeholder');
    if (placeholder && !isBlankText(placeholder)) return placeholder;
    // 4. Ancestor <label>
    const parentLabel = node.closest('label');
    if (parentLabel) {
      const t = parentLabel.innerText.trim();
      if (!isBlankText(t)) return t;
    }
    // 5. Yandex Forms: name="answer_*" → walk DOM ancestors for question title div
    const name = node.getAttribute('name') || '';
    if (name.startsWith('answer_')) {
      const yLabel = yandexAncestorLabel(node);
      if (!isBlankText(yLabel)) return yLabel;
    }
    // 6. Nearest question block inner text
    const block = node.closest('[role="listitem"], .freebirdFormviewerComponentsQuestionBaseRoot, div');
    if (block) {
      const t = compact(block.innerText).slice(0, 300);
      if (!isBlankText(t)) return t;
    }
    return '';
  }
  function fieldType(node) {
    const tag = node.tagName.toLowerCase();
    const role = node.getAttribute('role');
    const type = (node.getAttribute('type') || '').toLowerCase();
    if (node.getAttribute('contenteditable') === 'true') return 'text';
    if (tag === 'textarea' || role === 'textbox') return tag === 'textarea' ? 'textarea' : 'text';
    if (tag === 'select') return 'select';
    if (role === 'radio' || type === 'radio') return 'radio';
    if (role === 'checkbox' || type === 'checkbox') return 'checkbox';
    if (type === 'email') return 'email';
    if (type === 'tel') return 'tel';
    if (tag === 'input') return type || 'text';
    return 'unknown';
  }
  function isVisible(node) {
    const style = window.getComputedStyle(node);
    if (style.display === 'none' || style.visibility === 'hidden' || style.opacity === '0') return false;
    const rect = node.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return false;
    return true;
  }
  function isCaptchaNode(node) {
    const text = (node.innerText || node.getAttribute('aria-label') || '').toLowerCase();
    if (text.includes('captcha') || text.includes('smartcaptcha') || text.includes('recaptcha') || text.includes('hcaptcha')) return true;
    let cur = node;
    while (cur && cur !== document.body) {
      const cls = String(cur.className || '').toLowerCase();
      const id = String(cur.id || '').toLowerCase();
      const data = Array.from(cur.attributes || []).map(a => `${a.name}=${a.value}`).join(' ').toLowerCase();
      if (cls.includes('captcha') || id.includes('captcha') || data.includes('captcha')) return true;
      cur = cur.parentElement;
    }
    return false;
  }
  // Parse Yandex Forms name attribute: "answer_short_text_1685088" → {type: "answer_short_text", id: "1685088"}
  function parseYandexName(name) {
    const m = name.match(/^(answer_[a-z_]+?)_(\d+)$/);
    return m ? {apiAnswerType: m[1], yandexQuestionId: m[2]} : {apiAnswerType: '', yandexQuestionId: ''};
  }
  const nodes = Array.from(document.querySelectorAll(
    'input:not([type="hidden"]), textarea, select, [role="textbox"], [role="radio"], [role="checkbox"]'
  ));
  const seen = new Set();
  const result = [];
  const groups = new Map();
  for (const node of nodes) {
    if (!isVisible(node)) continue;
    if (isCaptchaNode(node)) continue;
    const rawType = (node.getAttribute('type') || '').toLowerCase();
    const role = (node.getAttribute('role') || '').toLowerCase();
    if (rawType === 'radio' || rawType === 'checkbox' || role === 'radio' || role === 'checkbox') {
      const container = nearestQuestion(node);
      const name = node.getAttribute('name') || node.getAttribute('aria-name') || '';
      const groupKey = `${rawType || role}:${name || cssPath(container)}`;
      const groupType = (rawType === 'radio' || role === 'radio') ? 'radio_group' : 'checkbox_group';
      const {apiAnswerType, yandexQuestionId} = parseYandexName(name);
      if (!groups.has(groupKey)) {
        const lbl = containerLabel(container, node);
        groups.set(groupKey, {
          id: name || groupKey,
          selector: cssPath(container),
          label: lbl,
          type: groupType,
          required: false,
          options: [],
          normalized_label: normalized(lbl),
          question_block_text: compact(container.innerText).slice(0, 1000),
          placeholder: '',
          aria_label: node.getAttribute('aria-label') || '',
          nearby_text: compact((node.parentElement && node.parentElement.innerText) || '').slice(0, 500),
          yandex_question_id: yandexQuestionId,
          api_answer_type: apiAnswerType,
          api_question_id: name,
        });
      }
      const group = groups.get(groupKey);
      group.required = group.required || Boolean(node.required || node.getAttribute('aria-required') === 'true');
      const label = optionLabel(node) || node.value || node.id || cssPath(node);
      if (label && !group.options.some(opt => opt.label === label || opt.value === node.value)) {
        group.options.push({label, value: node.value || label, selector: cssPath(node), id: node.id || ''});
      }
      continue;
    }
    const selector = cssPath(node);
    if (!selector || seen.has(selector)) continue;
    seen.add(selector);
    const id = node.id || node.name || node.getAttribute('aria-label') || selector;
    const options = node.tagName.toLowerCase() === 'select'
      ? Array.from(node.options).map(o => ({label: compact(o.text), value: o.value || compact(o.text), selector: selector, id: o.id || ''})).filter(o => o.label || o.value)
      : [];
    const label = labelFor(node);
    const container = nearestQuestion(node);
    const fieldName = node.getAttribute('name') || '';
    const {apiAnswerType, yandexQuestionId} = parseYandexName(fieldName);
    result.push({
      id,
      selector,
      label,
      normalized_label: normalized(label),
      type: fieldType(node),
      required: Boolean(node.required || node.getAttribute('aria-required') === 'true'),
      options,
      placeholder: node.getAttribute('placeholder') || '',
      aria_label: node.getAttribute('aria-label') || '',
      nearby_text: compact((node.parentElement && node.parentElement.innerText) || '').slice(0, 500),
      question_block_text: compact(container.innerText).slice(0, 1000),
      yandex_question_id: yandexQuestionId,
      api_answer_type: apiAnswerType,
      api_question_id: fieldName || id,
    });
  }
  for (const group of groups.values()) {
    if (group.options.length === 1 && group.type === 'checkbox_group') {
      group.type = 'checkbox';
    }
    group.yandex_option_ids = group.options.map(o => o.value).filter(Boolean);
    result.push(group);
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
  <fieldset><legend>Интересы</legend>
    <label><input type="checkbox" name="interests" value="events">Мероприятия</label>
    <label><input type="checkbox" name="interests" value="career">Карьера</label>
    <label><input type="checkbox" name="interests" value="research">Исследования</label>
  </fieldset>
  <label>Комментарий <textarea name="comment"></textarea></label><br>
  <button type="submit">Отправить</button>
</form>
</body></html>
""")


@app.get("/demo-thanks")
async def demo_thanks() -> Any:
    from fastapi.responses import HTMLResponse
    return HTMLResponse("<!doctype html><html><body><h1>Спасибо</h1><p>Form submitted.</p></body></html>")


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
  <fieldset><legend>Оценка</legend>
    <label><input type="radio" name="rating" value="5">5</label>
    <label><input type="radio" name="rating" value="4">4</label>
    <label><input type="radio" name="rating" value="3">3</label>
  </fieldset>
  <fieldset><legend>Интересы</legend>
    <label><input type="checkbox" name="interests" value="events">Мероприятия</label>
    <label><input type="checkbox" name="interests" value="career">Карьера</label>
  </fieldset>
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


@app.get("/demo-captcha")
async def demo_captcha() -> Any:
    from fastapi.responses import HTMLResponse
    return HTMLResponse("""
<!doctype html><html><body>
<h1>Captcha demo</h1>
<form>
  <div class="SmartCaptcha" data-testid="smartcaptcha">
    <label><input type="checkbox" name="smartcaptcha"> I am not a robot</label>
  </div>
</form>
</body></html>
""")


@app.post("/inspect-form")
async def inspect_form(req: InspectRequest) -> dict[str, Any]:
    await cleanup_expired_sessions()
    sid = req.session_id or str(uuid.uuid4())
    try:
      playwright = await async_playwright().start()
      browser, context = await create_stealth_context(playwright)
      page = await context.new_page()
      await page.goto(req.url, wait_until="domcontentloaded", timeout=60000)
      try:
          await page.wait_for_load_state("networkidle", timeout=20000)
      except Exception:
          pass
      for selector in [
          'input', 'textarea', 'select', '[role="radio"]', '[role="checkbox"]',
          '[role="textbox"]', '[contenteditable="true"]', 'button'
      ]:
          try:
              await page.locator(selector).first.wait_for(timeout=3000)
              break
          except Exception:
              pass
      if "yandex" in (req.url or "").lower():
          await wait_for_yandex_forms(page)
      auth_required = await detect_auth_required(page)
      captcha_required = await detect_captcha_required(page)
      shot = await screenshot(page, sid, "inspect")
      fields: list[dict[str, Any]] = []
      extraction_errors: list[str] = []
      extraction_strategy_used = "captcha_blocked" if captcha_required else ("auth_required" if auth_required else "dom_controls")
      if not auth_required and not captcha_required:
          for _ in range(3):
              try:
                  fields = await page.evaluate(FIELD_SCRIPT)
              except Exception as exc:
                  extraction_errors.append(str(exc))
                  fields = []
              if fields:
                  break
              await page.wait_for_timeout(1000)
      visible_text = ""
      try:
          visible_text = await page.locator("body").inner_text(timeout=3000)
      except Exception as exc:
          extraction_errors.append(str(exc))
      if not auth_required and not captcha_required and not fields and visible_text:
          fields = infer_virtual_fields_from_text(visible_text)
          if fields:
              extraction_strategy_used = "visible_text_virtual_fields"
      debug_info: dict[str, Any] = {}
      if req.debug:
          async def count(selector: str) -> int:
              try:
                  return await page.locator(selector).count()
              except Exception:
                  return 0
          debug_info = {
              "page_title": await page.title(),
              "final_url": page.url,
              "visible_text_sample": visible_text[:2000],
              "input_count": await count('input:not([type="hidden"])'),
              "textarea_count": await count('textarea'),
              "select_count": await count('select'),
              "role_radio_count": await count('[role="radio"], input[type="radio"]'),
              "role_checkbox_count": await count('[role="checkbox"], input[type="checkbox"]'),
              "button_count": await count('button, [role="button"], input[type="submit"]'),
              "candidate_question_blocks": len(fields),
              "extraction_strategy_used": extraction_strategy_used,
              "extraction_errors": extraction_errors,
              "form_type": form_type_for(page.url or req.url),
              "auth_required": auth_required,
              "captcha_required": captcha_required,
              "screenshot_path": shot,
              "error": "captcha_required" if captcha_required else ("" if fields or auth_required else "no interactive form fields found"),
          }
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
          "captcha_required": captcha_required,
          "fields": fields,
          "screenshot_path": shot,
          "debug": debug_info,
          "error": "captcha_required" if captcha_required else ("" if fields or auth_required else "no interactive form fields found"),
      }
    except Exception as exc:
      return {"ok": False, "session_id": sid, "url": req.url, "fields": [], "captcha_required": False, "error": str(exc)}


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
    radio_count = await loc.locator('input[type="radio"], [role="radio"]').count()
    checkbox_count = await loc.locator('input[type="checkbox"], [role="checkbox"]').count()
    if radio_count:
        target = value.strip().lower()
        for i in range(radio_count):
            opt = loc.locator('input[type="radio"], [role="radio"]').nth(i)
            label = ""
            try:
                label = await opt.evaluate(r"""el => {
                  const c = x => (x || '').replace(/\s+/g, ' ').trim();
                  if (el.labels && el.labels.length) return c(Array.from(el.labels).map(x => x.innerText).join(' '));
                  const aria = el.getAttribute('aria-label');
                  if (aria) return aria;
                  const parent = el.closest('label') || el.parentElement;
                  return parent ? c(parent.innerText) : (el.value || '');
                }""")
            except Exception:
                label = ""
            raw = ((await opt.get_attribute("value")) or "").strip().lower()
            if label.strip().lower() == target or raw == target or target in label.strip().lower():
                try:
                    await opt.check(force=True)
                except Exception:
                    await opt.click(force=True)
                return
        raise RuntimeError("radio option not found")
    if checkbox_count and tag != "input":
        values = [x.strip().lower() for x in re.split(r"[,;]", value) if x.strip()]
        if value.strip().startswith("[") and value.strip().endswith("]"):
            values = [x.strip().strip('"\'').lower() for x in value.strip()[1:-1].split(",") if x.strip()]
        for i in range(checkbox_count):
            opt = loc.locator('input[type="checkbox"], [role="checkbox"]').nth(i)
            label = ""
            try:
                label = await opt.evaluate(r"""el => {
                  const c = x => (x || '').replace(/\s+/g, ' ').trim();
                  if (el.labels && el.labels.length) return c(Array.from(el.labels).map(x => x.innerText).join(' '));
                  const aria = el.getAttribute('aria-label');
                  if (aria) return aria;
                  const parent = el.closest('label') || el.parentElement;
                  return parent ? c(parent.innerText) : (el.value || '');
                }""")
            except Exception:
                label = ""
            raw = ((await opt.get_attribute("value")) or "").strip().lower()
            hay = label.strip().lower()
            if any(v == raw or v == hay or v in hay for v in values):
                try:
                    await opt.check(force=True)
                except Exception:
                    await opt.click(force=True)
        return
    if tag == "select":
        try:
            await loc.select_option(label=value)
        except Exception:
            try:
                await loc.select_option(value=value)
            except Exception:
                if value.isdigit():
                    await loc.select_option(index=int(value))
                else:
                    raise
        return
    if typ == "file":
        raise RuntimeError("file upload requires manual handling")
    try:
        contenteditable = await loc.get_attribute("contenteditable")
    except Exception:
        contenteditable = None
    if contenteditable == "true":
        await loc.evaluate(
            """(el, value) => {
                el.textContent = value;
                el.dispatchEvent(new InputEvent('input', {bubbles: true, inputType: 'insertText', data: value}));
                el.dispatchEvent(new Event('change', {bubbles: true}));
            }""",
            value,
        )
        return
    if typ in {"checkbox"} or role == "checkbox":
        if value.lower() in {"1", "true", "yes", "да", "on"}:
            try:
                await loc.check(force=True)
            except Exception:
                await loc.click(force=True)
        else:
            try:
                await loc.uncheck(force=True)
            except Exception:
                pass
        return
    if typ == "radio" or role == "radio":
        try:
            await loc.check(force=True)
        except Exception:
            await loc.click(force=True)
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
            'input[type="email"]:visible',
            'input[autocomplete="username"]:visible',
            'input[autocomplete="email"]:visible',
            'input[name*="email" i]:visible',
            'input[name*="login" i]:visible',
            'input[name*="user" i]:visible',
            'input[id*="email" i]:visible',
            'input[id*="login" i]:visible',
            'input[id*="user" i]:visible',
        ]
        password_selectors = ['input[type="password"]:visible', 'input[autocomplete="current-password"]:visible']
        button_selectors = [
            'button:visible:has-text("Войти")', 'button:visible:has-text("Sign in")',
            'button:visible:has-text("Login")', 'button:visible:has-text("Log in")',
            'button:visible:has-text("Continue")', 'button:visible:has-text("Продолжить")',
            'button:visible:has-text("Далее")', 'button:visible:has-text("Submit")',
            'button:visible:has-text("Подтвердить")',
            '[role="button"]:visible:has-text("Войти")', '[role="button"]:visible:has-text("Sign in")'
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
            'input[autocomplete="one-time-code"]:visible',
            'input[name*="code" i]:visible', 'input[id*="code" i]:visible',
            'input[name*="otp" i]:visible', 'input[id*="otp" i]:visible',
            'input[type="tel"]:visible', 'input[inputmode="numeric"]:visible',
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
        for selector in ['button:visible:has-text("Подтвердить")', 'button:visible:has-text("Continue")', 'button:visible:has-text("Submit")', 'button:visible:has-text("Далее")', '[role="button"]:visible:has-text("Подтвердить")']:
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
    for selector in ['input[autocomplete="one-time-code"]:visible', 'input[name*="code" i]:visible', 'input[id*="otp" i]:visible']:
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
    captcha_required = await detect_captcha_required(page)
    shot = await screenshot(page, req.session_id, "reinspect")
    fields: list[dict[str, Any]] = []
    if not auth_required and not captcha_required:
        fields = await page.evaluate(FIELD_SCRIPT)
    return {
        "ok": True,
        "session_id": req.session_id,
        "url": page.url,
        "final_url": page.url,
        "title": await page.title(),
        "form_type": form_type_for(page.url),
        "auth_required": auth_required,
        "captcha_required": captcha_required,
        "fields": fields,
        "screenshot_path": shot,
        "error": "captcha_required" if captcha_required else "",
    }


@app.post("/close-session")
async def close_session(req: CloseSessionRequest) -> dict[str, Any]:
    await cleanup_expired_sessions()
    closed = await close_session_id(req.session_id)
    return {"ok": True, "closed": closed, "error": ""}


@app.get("/session/{session_id}/screenshot")
async def get_session_screenshot(session_id: str):
    from fastapi.responses import Response
    await cleanup_expired_sessions()
    session = touch_session(session_id)
    if not session:
        return Response(content=b"session not found", status_code=404, media_type="text/plain")
    page: Page = session["page"]
    try:
        png_bytes = await page.screenshot(type="png")
        return Response(content=png_bytes, media_type="image/png")
    except Exception as exc:
        return Response(content=str(exc).encode(), status_code=500, media_type="text/plain")


@app.post("/session/{session_id}/click")
async def click_at_session(session_id: str, req: ClickRequest) -> dict[str, Any]:
    await cleanup_expired_sessions()
    session = touch_session(session_id)
    if not session:
        return {"ok": False, "error": "session not found"}
    page: Page = session["page"]
    try:
        await page.mouse.click(req.x, req.y)
        shot = await screenshot(page, session_id, "click")
        return {"ok": True, "x": req.x, "y": req.y, "screenshot_path": shot, "error": ""}
    except Exception as exc:
        return {"ok": False, "error": str(exc)}


@app.get("/demo-captcha-then-form")
async def demo_captcha_then_form():
    """Demo page that first shows a fake captcha placeholder, then reveals a form."""
    from fastapi.responses import HTMLResponse
    html = """<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <title>Demo Captcha then Form</title>
  <style>
    body { font-family: system-ui, sans-serif; max-width: 600px; margin: 4rem auto; padding: 1rem; }
    #captcha-block { border: 2px solid #e00; padding: 2rem; border-radius: 8px; margin-bottom: 2rem; }
    #form-block { display: none; border: 2px solid #090; padding: 2rem; border-radius: 8px; }
    button { padding: .5rem 1rem; font-size: 1rem; cursor: pointer; }
    input { display: block; margin: .5rem 0 1rem; padding: .4rem; width: 100%; box-sizing: border-box; }
  </style>
</head>
<body>
  <h1>Demo: Captcha then Form</h1>
  <div id="captcha-block">
    <h2>Вы не робот?</h2>
    <p>SmartCaptcha: нажмите кнопку для подтверждения.</p>
    <button id="pass-btn" onclick="passCaptcha()">✓ Я не робот</button>
  </div>
  <div id="form-block">
    <h2>Форма (доступна после капчи)</h2>
    <form id="demo-form">
      <label>ФИО<input type="text" name="full_name" required></label>
      <label>Email<input type="email" name="email" required></label>
      <label>Группа<input type="text" name="group" required></label>
      <button type="submit">Отправить</button>
    </form>
  </div>
  <script>
    function passCaptcha() {
      document.getElementById('captcha-block').style.display = 'none';
      document.getElementById('form-block').style.display = 'block';
    }
    document.getElementById('demo-form').addEventListener('submit', function(e) {
      e.preventDefault();
      document.body.innerHTML = '<h1>Форма отправлена!</h1><p>Демо завершено.</p>';
    });
  </script>
</body>
</html>"""
    return HTMLResponse(content=html)
