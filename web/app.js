(() => {
  const state = {
    status: {},
    config: {},
    events: [],
    forms: [],
    selectedFormId: "",
    selectedForm: null,
    profile: {},
    rulesText: "",
    validation: null,
    tests: {}
  };

  const profileFields = [
    ["full_name", "ФИО"],
    ["hse_email", "HSE email"],
    ["personal_email", "Personal email"],
    ["phone", "Телефон"],
    ["student_group", "Группа"],
    ["faculty", "Факультет"],
    ["programme", "Программа"],
    ["course_year", "Курс"],
    ["campus", "Кампус"]
  ];

  const statusLabels = {
    waiting_user_review: "Проверьте поля",
    waiting_auth: "Нужен вход",
    waiting_2fa: "Нужен код подтверждения",
    waiting_submit_confirm: "Готово к отправке",
    submitted: "Отправлено",
    manual_required: "Нужно вручную",
    failed: "Ошибка",
    cancelled: "Отменено"
  };

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

  function escapeHtml(value) {
    return String(value ?? "").replace(/[&<>"']/g, ch => ({
      "&": "&amp;",
      "<": "&lt;",
      ">": "&gt;",
      "\"": "&quot;",
      "'": "&#39;"
    }[ch]));
  }

  function authToken() {
    return new URLSearchParams(window.location.search).get("token") || "";
  }

  function withToken(path) {
    const token = authToken();
    if (!token) return path;
    return `${path}${path.includes("?") ? "&" : "?"}token=${encodeURIComponent(token)}`;
  }

  async function api(path, options = {}, settings = {}) {
    const opts = { ...options };
    opts.headers = { ...(opts.headers || {}) };
    const token = authToken();
    if (token) opts.headers["X-Auth-Token"] = token;
    if (opts.body && !(opts.body instanceof FormData) && !opts.headers["Content-Type"]) {
      opts.headers["Content-Type"] = "application/json";
    }

    const response = await fetch(withToken(path), opts);
    const text = await response.text();
    let data = text;
    try {
      data = text ? JSON.parse(text) : {};
    } catch (_) {
      data = { ok: response.ok, text };
    }
    if (!response.ok || (data?.ok === false && !settings.allowOkFalse)) {
      throw new Error(normalizeApiError(data, response.statusText));
    }
    return data;
  }

  function normalizeApiError(data, fallback = "Request failed") {
    if (!data) return fallback;
    if (typeof data === "string") return data || fallback;
    if (data.error) return data.error;
    if (data.message && data.ok === false) return data.message;
    if (data.details?.error) return data.details.error;
    return fallback;
  }

  function normalizeArrayResponse(data, key) {
    if (Array.isArray(data)) return data;
    if (Array.isArray(data?.[key])) return data[key];
    if (Array.isArray(data?.data)) return data.data;
    if (Array.isArray(data?.data?.[key])) return data.data[key];
    return [];
  }

  function normalizeForm(data) {
    if (!data) return null;
    if (data.form) return data.form;
    if (data.data?.form) return data.data.form;
    if (data.data?.id || data.data?.fields) return data.data;
    return data;
  }

  function normalizeOptions(field) {
    return (field?.options || []).map(option => {
      if (typeof option === "string") return { label: option, value: option, id: option };
      return {
        label: option.label || option.value || option.id || "",
        value: option.value || option.label || option.id || "",
        id: option.id || option.value || option.label || "",
        selector: option.selector || ""
      };
    }).filter(option => option.label || option.value || option.id);
  }

  function resultPayload(data) {
    return data?.data || data;
  }

  function badge(label, tone = "neutral") {
    return `<span class="badge badge--${tone}">${escapeHtml(label)}</span>`;
  }

  function toneForStatus(status) {
    if (["submitted"].includes(status)) return "ok";
    if (["waiting_user_review", "waiting_submit_confirm"].includes(status)) return "warn";
    if (["waiting_auth", "waiting_2fa", "manual_required"].includes(status)) return "info";
    if (["failed", "cancelled"].includes(status)) return "error";
    return "neutral";
  }

  function toneForConfidence(confidence) {
    const value = Number(confidence || 0);
    if (value >= 0.85) return "ok";
    if (value >= 0.6) return "warn";
    return "muted";
  }

  function humanStatus(status) {
    return statusLabels[status] || status || "unknown";
  }

  function summarizeForm(form) {
    const fields = form?.fields || [];
    const ready = fields.filter(field => hasFieldValue(field) && !field.validation_error).length;
    const needsInput = fields.filter(field => field.requires_user_input).length;
    const missingRequired = fields.filter(field => field.required && !hasFieldValue(field)).length;
    const unsupported = fields.filter(field => field.can_auto_fill === false).length;
    return { ready, needsInput, missingRequired, unsupported, total: fields.length };
  }

  function hasFieldValue(field) {
    return Boolean(field?.value) || Boolean(field?.values && field.values.length);
  }

  function showToast(message, tone = "ok") {
    const item = document.createElement("div");
    item.className = `toast toast--${tone}`;
    item.textContent = message;
    $("#toast-stack").appendChild(item);
    window.setTimeout(() => item.remove(), 5200);
  }

  function showResult(id, data, tone = "") {
    const el = $(id);
    if (!el) return;
    el.className = `result-panel ${tone ? `result-panel--${tone}` : ""}`;
    if (typeof data === "string") {
      el.textContent = data;
      return;
    }
    el.innerHTML = `<pre>${escapeHtml(JSON.stringify(data, null, 2))}</pre>`;
  }

  function switchSection(name) {
    $$(".nav__item").forEach(item => item.classList.toggle("is-active", item.dataset.section === name));
    $$(".section").forEach(section => section.classList.toggle("is-active", section.id === `section-${name}`));
  }

  async function refreshAll() {
    await Promise.allSettled([
      loadStatus(),
      loadForms(),
      loadEvents(),
      loadConfig(),
      loadProfile(),
      loadRules()
    ]);
    renderAll();
  }

  async function loadStatus() {
    state.status = await api("/api/status").catch(error => ({ error: error.message }));
  }

  async function loadForms() {
    const all = $("#show-all-forms")?.checked ? "?all=true" : "";
    const data = await api(`/api/forms/active${all}`).catch(() => []);
    state.forms = normalizeArrayResponse(data, "forms").map(normalizeForm).filter(Boolean);
    if (state.selectedFormId) {
      await loadSelectedForm(state.selectedFormId, false);
    }
  }

  async function loadSelectedForm(id, render = true) {
    const data = await api(`/api/forms/${encodeURIComponent(id)}`);
    state.selectedForm = normalizeForm(data);
    state.selectedFormId = state.selectedForm?.id || id;
    state.validation = null;
    if (render) {
      renderForms();
      renderReview();
      switchSection("review");
    }
  }

  async function loadEvents() {
    const data = await api("/api/events?limit=40").catch(() => []);
    state.events = normalizeArrayResponse(data, "events");
  }

  async function loadConfig() {
    state.config = await api("/api/config").catch(error => ({ error: error.message }));
  }

  async function loadProfile() {
    const data = await api("/api/profile").catch(() => ({}));
    state.profile = resultPayload(data) || {};
  }

  async function loadRules() {
    const response = await fetch(withToken("/api/rules"));
    state.rulesText = await response.text();
  }

  function renderAll() {
    renderDashboard();
    renderForms();
    renderReview();
    renderEvents();
    renderConfig();
    renderProfile();
    renderRules();
  }

  function renderDashboard() {
    const status = state.status || {};
    const mailboxes = status.mailboxes_status || [];
    const browser = status.browser_worker || {};
    const telegram = status.telegram || {};
    const llm = status.llm || {};
    const cards = [
      {
        title: "Mail",
        status: mailboxes.length ? "ok" : "skipped",
        value: `${mailboxes.length || 0} mailbox`,
        detail: mailboxes.map(m => `${m.id}: ${m.error || "configured"}`).join("\n") || "Run Test IMAP to verify credentials."
      },
      {
        title: "Telegram",
        status: telegram.enabled ? "ok" : "skipped",
        value: telegram.enabled ? "enabled" : "disabled",
        detail: telegram.proxy_configured ? `Proxy: ${telegram.proxy_url_redacted || "configured"}` : "Proxy not configured"
      },
      {
        title: "BrowserWorker",
        status: browser.endpoint ? "ok" : "warning",
        value: browser.endpoint || status.browser_worker_endpoint || "unknown",
        detail: "Run Test Browser for demo-form extraction."
      },
      {
        title: "LLM",
        status: llm.enabled ? "ok" : "skipped",
        value: llm.enabled ? `${llm.provider || "ollama"} / ${llm.model || ""}` : "Noop fallback",
        detail: llm.endpoint || "Rule-based mapping active."
      },
      {
        title: "Active forms",
        status: state.forms.length ? "warning" : "ok",
        value: String(state.forms.length),
        detail: state.forms.length ? "Open Active Forms to continue." : "No forms waiting now."
      },
      {
        title: "Last events",
        status: state.events.some(e => e.level === "error") ? "error" : "ok",
        value: String(state.events.length),
        detail: state.events.slice(0, 3).map(e => e.message || e.type).join("\n") || "No recent events."
      }
    ];
    $("#dashboard-cards").innerHTML = cards.map(card => `
      <article class="metric-card">
        <div class="metric-card__top">
          <span>${escapeHtml(card.title)}</span>
          ${badge(card.status.toUpperCase(), card.status === "warning" ? "warn" : card.status)}
        </div>
        <strong>${escapeHtml(card.value)}</strong>
        <p>${escapeHtml(card.detail)}</p>
      </article>
    `).join("");
  }

  function renderForms() {
    const list = $("#forms-list");
    if (!state.forms.length) {
      list.innerHTML = `<div class="empty-state">Активных форм нет. Создайте demo или вставьте URL Яндекс Формы.</div>`;
      return;
    }
    list.innerHTML = state.forms.map(form => {
      const summary = summarizeForm(form);
      const selected = form.id === state.selectedFormId ? " is-selected" : "";
      return `
        <article class="form-card${selected}">
          <div class="form-card__main">
            <div class="form-card__title">${escapeHtml(form.title || form.form_url || form.id)}</div>
            <div class="form-card__meta">
              ${badge(humanStatus(form.status), toneForStatus(form.status))}
              ${badge(form.form_type || "unknown", "neutral")}
              <span>${escapeHtml(domainFromUrl(form.form_url))}</span>
            </div>
            <div class="progress-line">
              <span>ready ${summary.ready}/${summary.total}</span>
              <span>needs input ${summary.needsInput}</span>
              <span>required missing ${summary.missingRequired}</span>
              <span>unsupported ${summary.unsupported}</span>
            </div>
          </div>
          <div class="form-card__actions">
            <button class="button button--primary" data-form-review="${escapeHtml(form.id)}" type="button">Review</button>
            <a class="button button--ghost" href="${escapeHtml(form.form_url || "#")}" target="_blank" rel="noreferrer">Open original</a>
            <button class="button button--danger" data-form-cancel="${escapeHtml(form.id)}" type="button">Cancel</button>
          </div>
        </article>
      `;
    }).join("");
  }

  function renderReview() {
    const form = state.selectedForm;
    $("#review-empty").hidden = Boolean(form);
    $("#review-content").hidden = !form;
    if (!form) return;

    const summary = summarizeForm(form);
    $("#review-summary").innerHTML = `
      <h3>${escapeHtml(form.title || form.id)}</h3>
      <p>${escapeHtml(form.form_url || "")}</p>
      <div class="summary-grid">
        <div><strong>${summary.ready}</strong><span>Готово</span></div>
        <div><strong>${summary.needsInput}</strong><span>Нужно заполнить</span></div>
        <div><strong>${summary.missingRequired}</strong><span>Обязательных пустых</span></div>
        <div><strong>${lowConfidenceCount(form)}</strong><span>Низкая уверенность</span></div>
        <div><strong>${summary.unsupported}</strong><span>Нельзя автоматически</span></div>
      </div>
      <div class="stack">
        ${badge(humanStatus(form.status), toneForStatus(form.status))}
        ${badge(form.form_type || "unknown", "neutral")}
      </div>
    `;

    $("#open-original-button").disabled = !form.form_url;
    $("#submit-button").hidden = form.status !== "waiting_submit_confirm";
    $("#fill-button").hidden = form.status !== "waiting_user_review";
    $("#save-fields-button").disabled = !["waiting_user_review", "waiting_submit_confirm"].includes(form.status);
    $("#remap-button").disabled = form.status !== "waiting_user_review";
    $("#validate-button").disabled = !form.fields?.length;

    renderAuthCard(form);
    $("#field-list").innerHTML = (form.fields || []).map((field, index) => renderFieldEditor(field, index, form)).join("");
    renderValidationSummary();
  }

  function renderAuthCard(form) {
    const card = $("#auth-card");
    if (!["waiting_auth", "waiting_2fa"].includes(form.status)) {
      card.hidden = true;
      card.innerHTML = "";
      return;
    }
    card.hidden = false;
    if (form.status === "waiting_auth") {
      card.innerHTML = `
        <h3>Нужен вход</h3>
        <p>Логин и пароль вводятся только локально в Web UI. Telegram не принимает пароли.</p>
        <div class="form-grid">
          <label class="field"><span>Username / email</span><input id="auth-username" autocomplete="username"></label>
          <label class="field"><span>Password</span><input id="auth-password" type="password" autocomplete="current-password"></label>
        </div>
        <div class="button-row">
          <button class="button button--primary" id="auth-login-button" type="button">Login</button>
          <button class="button" id="auth-reinspect-button" type="button">Reinspect after manual login</button>
        </div>
      `;
    } else {
      card.innerHTML = `
        <h3>Нужен код подтверждения</h3>
        <p>Код можно ввести здесь или отправить в Telegram, если 2FA через Telegram включён.</p>
        <label class="field"><span>2FA code</span><input id="auth-code" inputmode="numeric" autocomplete="one-time-code"></label>
        <div class="button-row">
          <button class="button button--primary" id="auth-2fa-button" type="button">Submit 2FA</button>
          <button class="button" id="auth-reinspect-button" type="button">Reinspect</button>
        </div>
      `;
    }
  }

  function renderFieldEditor(field, index, form) {
    const id = `field-${index}`;
    const options = normalizeOptions(field);
    const value = field.value || "";
    const readonly = !["waiting_user_review", "waiting_submit_confirm"].includes(form.status);
    const disabled = readonly ? "disabled" : "";
    const source = field.source || "unknown";
    const confidence = Number(field.confidence || 0);
    const validationError = field.validation_error || validationErrorForField(field.id);
    const question = field.question_block_text || field.nearby_text || field.label || field.id;
    const semanticWarning = semanticWarningForField(field);

    let editor = "";
    if (field.can_auto_fill === false) {
      editor = `<div class="warning-box">${escapeHtml(field.unsupported_reason || "Поле требует ручной обработки.")}</div>`;
    } else if (field.type === "textarea") {
      editor = `<textarea id="${id}" data-field-id="${escapeHtml(field.id)}" ${disabled}>${escapeHtml(value)}</textarea>`;
    } else if (field.type === "radio_group" || field.type === "select") {
      editor = `
        <select id="${id}" data-field-id="${escapeHtml(field.id)}" ${disabled}>
          <option value=""></option>
          ${options.map(option => `<option value="${escapeHtml(option.value || option.label)}" ${String(option.value || option.label) === String(value) ? "selected" : ""}>${escapeHtml(option.label || option.value)}</option>`).join("")}
        </select>
      `;
    } else if (field.type === "checkbox_group") {
      const selected = new Set(field.values?.length ? field.values : splitMultiValue(value));
      editor = `<div class="option-list">${options.map(option => {
        const optionValue = option.value || option.label;
        return `
          <label class="check">
            <input type="checkbox" data-field-id="${escapeHtml(field.id)}" data-group="${id}" value="${escapeHtml(optionValue)}" ${selected.has(optionValue) || selected.has(option.label) ? "checked" : ""} ${disabled}>
            <span>${escapeHtml(option.label || optionValue)}</span>
          </label>
        `;
      }).join("")}</div>`;
    } else if (field.type === "checkbox") {
      const checked = ["true", "yes", "да", "1", "on"].includes(String(value).toLowerCase());
      editor = `
        <label class="check check--consent">
          <input id="${id}" data-field-id="${escapeHtml(field.id)}" type="checkbox" ${checked ? "checked" : ""} ${disabled}>
          <span>Подтверждаю явно</span>
        </label>
      `;
    } else {
      const inputType = ["email", "tel", "number", "date"].includes(field.type) ? field.type : "text";
      editor = `<input id="${id}" data-field-id="${escapeHtml(field.id)}" type="${inputType}" value="${escapeHtml(value)}" ${disabled}>`;
    }

    return `
      <article class="field-card ${validationError ? "field-card--error" : ""}">
        <div class="field-card__head">
          <div>
            <div class="field-card__label">${escapeHtml(question)}</div>
          <div class="field-card__meta">
            ${badge(field.type || "text", "neutral")}
            ${field.required ? badge("required", "warn") : badge("optional", "muted")}
            ${badge(sourceLabel(field.source), sourceTone(field.source))}
            ${field.requires_user_input ? badge("needs input", "info") : ""}
          </div>
          </div>
          <div class="field-card__confidence">
            ${badge(`${Math.round(confidence * 100)}%`, toneForConfidence(confidence))}
          </div>
        </div>
        ${editor}
        <div class="field-card__foot">
          <span>source: ${escapeHtml(sourceLabel(source))}</span>
          <span>key: ${escapeHtml(field.mapped_profile_key || field.semantic_key || "unknown")}</span>
          <button class="link-button" data-explain-field="${escapeHtml(field.id)}" type="button">Почему?</button>
        </div>
        ${field.reason ? `<p class="hint">${escapeHtml(field.reason)}</p>` : ""}
        ${semanticWarning ? `<div class="warning-box">${escapeHtml(semanticWarning)}</div>` : ""}
        ${validationError ? `<div class="inline-error">${escapeHtml(validationError)}</div>` : ""}
      </article>
    `;
  }

  function lowConfidenceCount(form) {
    return (form?.fields || []).filter(field => Number(field.confidence || 0) > 0 && Number(field.confidence || 0) < 0.75).length;
  }

  function sourceLabel(source) {
    const labels = { profile: "Profile", rule: "Rule", llm: "LLM", user: "User", empty: "Empty" };
    return labels[source] || source || "Empty";
  }

  function sourceTone(source) {
    if (source === "llm") return "info";
    if (source === "user") return "ok";
    if (source === "rule" || source === "profile") return "neutral";
    return "muted";
  }

  function semanticWarningForField(field) {
    if (field.semantic_key === "consent") return "Согласие и персональные данные требуют явного подтверждения пользователя.";
    if (field.semantic_key === "rating") return "Оценка требует выбора пользователя, сервис не выбирает её молча.";
    if (field.semantic_key === "opinion") return field.required ? "Комментарий обязателен: заполните его вручную." : "Комментарий опционален и не блокирует заполнение.";
    if (field.validation_error) return field.validation_error;
    return "";
  }

  function splitMultiValue(value) {
    if (!value) return [];
    return String(value).split(/[;,]/).map(item => item.trim()).filter(Boolean);
  }

  function validationErrorForField(id) {
    const item = state.validation?.fields?.find(field => field.id === id);
    return item && !item.ok ? item.error : "";
  }

  function renderValidationSummary() {
    const el = $("#validation-summary");
    if (!state.validation) {
      el.hidden = true;
      return;
    }
    el.hidden = false;
    if (state.validation.valid || state.validation.can_fill) {
      el.className = "error-summary error-summary--ok";
      el.textContent = "Validation passed. Форму можно заполнить.";
      return;
    }
    el.className = "error-summary";
    el.innerHTML = `
      <strong>Нужно исправить перед заполнением:</strong>
      <ul>
        <li>Обязательных пустых: ${state.validation.missing_required || 0}</li>
        <li>Нужен ввод: ${state.validation.needs_input || 0}</li>
        <li>Неподдерживаемых: ${state.validation.unsupported || 0}</li>
        <li>Ошибок вариантов: ${state.validation.invalid_options || 0}</li>
      </ul>
    `;
  }

  function renderEvents() {
    const list = $("#events-list");
    if (!state.events.length) {
      list.innerHTML = `<div class="empty-state">Событий пока нет.</div>`;
      return;
    }
    list.innerHTML = state.events.map(event => `
      <article class="event event--${escapeHtml(event.level || "info")}">
        <div class="event__time">${escapeHtml(event.created_at || event.time || "")}</div>
        <div class="event__body">
          <div><strong>${escapeHtml(event.type || event.level || "event")}</strong> ${badge(event.level || "info", event.level === "error" ? "error" : "neutral")}</div>
          <p>${escapeHtml(event.message || "")}</p>
          ${event.data_json || event.data ? `<details><summary>details</summary><pre>${escapeHtml(event.data_json || JSON.stringify(event.data, null, 2))}</pre></details>` : ""}
        </div>
      </article>
    `).join("");
  }

  function renderConfig() {
    $("#config-view").textContent = JSON.stringify(state.config || {}, null, 2);
  }

  function renderProfile() {
    const root = $("#profile-fields");
    root.innerHTML = profileFields.map(([key, label]) => `
      <label class="field">
        <span>${escapeHtml(label)}</span>
        <input data-profile-key="${escapeHtml(key)}" value="${escapeHtml(state.profile?.[key] || "")}">
      </label>
    `).join("") + `
      <label class="field field--wide">
        <span>Custom JSON</span>
        <textarea id="profile-custom-json" spellcheck="false">${escapeHtml(JSON.stringify(state.profile?.custom || {}, null, 2))}</textarea>
      </label>
    `;
    $("#profile-json").value = JSON.stringify(state.profile || {}, null, 2);
  }

  function renderRules() {
    if (!$("#rules-json").matches(":focus")) $("#rules-json").value = state.rulesText || "";
  }

  function domainFromUrl(url) {
    try {
      return new URL(url).hostname;
    } catch (_) {
      return "";
    }
  }

  function collectFieldValues() {
    const form = state.selectedForm;
    if (!form) return [];
    return (form.fields || []).map((field, index) => {
      if (field.type === "checkbox_group") {
        const values = $$(`[data-group="field-${index}"]:checked`).map(input => input.value);
        return { id: field.id, value: values.join(", ") };
      }
      const input = $(`#field-${index}`);
      if (!input) return { id: field.id, value: field.value || "" };
      if (field.type === "checkbox") return { id: field.id, value: input.checked ? "true" : "" };
      return { id: field.id, value: input.value };
    });
  }

  async function saveFields() {
    if (!state.selectedFormId) return;
    await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/fields`, {
      method: "POST",
      body: JSON.stringify({ fields: collectFieldValues() })
    });
    await loadSelectedForm(state.selectedFormId);
    showToast("Поля сохранены");
  }

  async function remapFields() {
    showResult("#test-result", "Remap with AI/rules is running...");
    await saveFields();
    const data = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/remap`, {
      method: "POST",
      body: JSON.stringify({ force: false, use_llm: true })
    });
    const form = normalizeForm(data);
    if (form?.id) state.selectedForm = form;
    await loadSelectedForm(state.selectedFormId);
    showRemapDiff(data);
    showToast("Сопоставление обновлено");
  }

  function showRemapDiff(data) {
    const payload = resultPayload(data);
    const changed = (payload.diff || data.diff || []).filter(item => item.changed);
    const summary = payload.summary || data.summary || {};
    const el = $("#test-result");
    el.className = "result-panel result-panel--ok";
    el.innerHTML = `
      <strong>Remap complete</strong>
      <p>Ready: ${summary.ready ?? "-"} · Needs input: ${summary.needs_input ?? "-"} · Unsupported: ${summary.unsupported ?? "-"} · Low confidence: ${summary.low_confidence ?? "-"}</p>
      ${changed.length ? `<div class="diff-list">${changed.map(item => `
        <div>
          <strong>${escapeHtml(item.label || item.field_id)}</strong>
          <span>${escapeHtml(item.old_value || "(empty)")} → ${escapeHtml(item.new_value || "(empty)")}</span>
          <small>${escapeHtml(item.new_source || "")} · ${Math.round(Number(item.confidence || 0) * 100)}% · ${escapeHtml(item.reason || "")}</small>
        </div>
      `).join("")}</div>` : "<p>Изменений нет. User edits сохранены.</p>"}
    `;
  }

  async function validateFields() {
    showResult("#test-result", "Validation is running...");
    await saveFields();
    const data = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/validate`, { method: "POST" });
    state.validation = resultPayload(data);
    renderReview();
    showResult("#test-result", data, state.validation?.can_fill ? "ok" : "warn");
  }

  async function fillForm() {
    showResult("#test-result", "Validating before fill...");
    await saveFields();
    const validation = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/validate`, { method: "POST" });
    state.validation = resultPayload(validation);
    if (!state.validation?.can_fill) {
      renderReview();
      showToast("Сначала исправьте ошибки в полях", "warn");
      return;
    }
    const data = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/fill`, { method: "POST" });
    showResult("#test-result", data, "ok");
    await refreshAfterAction("Форма заполнена. Теперь нужно отдельно подтвердить отправку.");
  }

  async function submitForm() {
    const confirmed = await confirmAction("Отправить форму?", "После подтверждения browser-worker нажмёт кнопку отправки на сайте. Это отдельное необратимое действие.");
    if (!confirmed) return;
    const data = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/submit`, { method: "POST" });
    showResult("#test-result", data, "ok");
    await refreshAfterAction("Форма отправлена");
  }

  async function manualForm() {
    const data = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/manual`, { method: "POST" });
    showResult("#test-result", data, "warn");
    await refreshAfterAction("Форма переведена в ручной режим");
  }

  async function cancelForm() {
    const confirmed = await confirmAction("Отменить работу с формой?", "Сессия browser-worker будет закрыта, форма исчезнет из активного списка.");
    if (!confirmed) return;
    const data = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/cancel`, { method: "POST" });
    showResult("#test-result", data, "warn");
    state.selectedFormId = "";
    state.selectedForm = null;
    await refreshAll();
    switchSection("forms");
  }

  async function reinspectForm() {
    const data = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/reinspect`, { method: "POST" });
    showResult("#test-result", data, "ok");
    await refreshAfterAction("Форма повторно проверена");
  }

  async function authLogin() {
    const username = $("#auth-username")?.value || "";
    const passwordInput = $("#auth-password");
    const password = passwordInput?.value || "";
    if (passwordInput) passwordInput.value = "";
    const data = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/auth/credentials`, {
      method: "POST",
      body: JSON.stringify({ username, password, remember: false })
    });
    showResult("#test-result", data, "ok");
    await refreshAfterAction("Данные входа отправлены в browser-worker");
  }

  async function submit2fa() {
    const codeInput = $("#auth-code");
    const code = codeInput?.value || "";
    if (codeInput) codeInput.value = "";
    const data = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/auth/2fa`, {
      method: "POST",
      body: JSON.stringify({ code })
    });
    showResult("#test-result", data, "ok");
    await refreshAfterAction("2FA код отправлен");
  }

  async function refreshAfterAction(message) {
    await refreshAll();
    if (state.selectedFormId) await loadSelectedForm(state.selectedFormId);
    renderAll();
    showToast(message);
  }

  async function runTest(name) {
    const data = await api(`/api/test/${name}`, { method: "POST" }, { allowOkFalse: true });
    state.tests[name] = data;
    showResult("#test-result", data, data.ok ? "ok" : "warn");
    renderDashboard();
    return data;
  }

  async function testAll() {
    showResult("#test-result", "Running tests...");
    const names = ["browser", "telegram", "imap", "llm"];
    const results = {};
    for (const name of names) {
      try {
        results[name] = await runTest(name);
      } catch (error) {
        results[name] = { ok: false, error: error.message };
      }
    }
    showResult("#test-result", results, Object.values(results).every(item => item.ok) ? "ok" : "warn");
  }

  async function createDemoForm(authDemo = false) {
    const path = authDemo ? "/api/demo/create-auth" : "/api/demo/create";
    const data = await api(path, { method: "POST" });
    showResult("#test-result", data, "ok");
    await refreshAll();
    switchSection("forms");
  }

  async function inspectUrl() {
    const url = $("#manual-url").value.trim();
    if (!url) {
      showResult("#manual-result", "Введите URL формы.", "warn");
      return;
    }
    $("#open-manual-url").href = url;
    showResult("#manual-result", "Inspecting form...");
    const data = await api("/api/forms/inspect-url", {
      method: "POST",
      body: JSON.stringify({ url, debug: $("#manual-debug").checked })
    });
    showInspectResult(data);
  }

  async function createFormFromUrl() {
    const url = $("#manual-url").value.trim();
    if (!url) {
      showResult("#manual-result", "Введите URL формы.", "warn");
      return;
    }
    showResult("#manual-result", "Creating form session...");
    const data = await api("/api/forms/create-from-url", {
      method: "POST",
      body: JSON.stringify({ url, title: "Manual test form", debug: $("#manual-debug").checked })
    });
    showResult("#manual-result", data, data.ok ? "ok" : "warn");
    await refreshAll();
    const payload = resultPayload(data);
    if (payload?.session_id) {
      await loadSelectedForm(payload.session_id);
    } else {
      switchSection("forms");
    }
  }

  function showInspectResult(data) {
    const payload = resultPayload(data);
    const fields = payload.fields || [];
    const debug = payload.debug || {};
    $("#manual-result").innerHTML = `
      <div class="inspect-result">
        <div class="inspect-result__summary">
          ${badge(data.ok ? "OK" : "ERROR", data.ok ? "ok" : "error")}
          ${badge(payload.form_type || "unknown", "neutral")}
          ${payload.auth_required ? badge("auth required", "warn") : badge("no auth", "ok")}
          <span>${fields.length} fields</span>
        </div>
        ${payload.error ? `<div class="inline-error">${escapeHtml(payload.error)}</div>` : ""}
        <div class="field-preview">
          ${fields.map(field => `<div><strong>${escapeHtml(field.label || field.id)}</strong><span>${escapeHtml(field.type || "")}</span></div>`).join("") || "Поля не найдены."}
        </div>
        ${payload.screenshot_path ? `<p>Screenshot: <code>${escapeHtml(payload.screenshot_path)}</code></p>` : ""}
        <details>
          <summary>Debug info</summary>
          <pre>${escapeHtml(JSON.stringify(debug, null, 2))}</pre>
        </details>
      </div>
    `;
  }

  async function saveProfile() {
    let profile = {};
    try {
      profile = JSON.parse($("#profile-json").value || "{}");
    } catch (error) {
      showResult("#profile-result", `Advanced JSON is invalid: ${error.message}`, "error");
      return;
    }
    $$("[data-profile-key]").forEach(input => {
      profile[input.dataset.profileKey] = input.value;
    });
    try {
      profile.custom = JSON.parse($("#profile-custom-json").value || "{}");
    } catch (error) {
      showResult("#profile-result", `Custom JSON is invalid: ${error.message}`, "error");
      return;
    }
    const data = await api("/api/profile", { method: "POST", body: JSON.stringify(profile, null, 2) });
    showResult("#profile-result", data, "ok");
    await loadProfile();
    renderProfile();
  }

  async function saveRules() {
    const text = $("#rules-json").value;
    try {
      JSON.parse(text);
    } catch (error) {
      showResult("#rules-result", `JSON syntax error: ${error.message}`, "error");
      return;
    }
    const data = await api("/api/rules", { method: "POST", body: text, headers: { "Content-Type": "application/json" } });
    showResult("#rules-result", data, "ok");
    await loadRules();
  }

  function validateRulesJson() {
    try {
      JSON.parse($("#rules-json").value || "{}");
      showResult("#rules-result", "JSON is valid.", "ok");
    } catch (error) {
      showResult("#rules-result", `JSON syntax error: ${error.message}`, "error");
    }
  }

  function confirmAction(title, text) {
    const modal = $("#confirm-modal");
    $("#confirm-title").textContent = title;
    $("#confirm-text").textContent = text;
    return new Promise(resolve => {
      const handler = () => {
        modal.removeEventListener("close", handler);
        resolve(modal.returnValue === "ok");
      };
      modal.addEventListener("close", handler);
      modal.showModal();
    });
  }

  function bindEvents() {
    $$(".nav__item").forEach(button => button.addEventListener("click", () => switchSection(button.dataset.section)));
    $("#refresh-button").addEventListener("click", () => safeRun(refreshAll));
    $("#test-all-button").addEventListener("click", () => safeRun(testAll));
    $("#test-browser-button").addEventListener("click", () => safeRun(() => runTest("browser")));
    $("#test-telegram-button").addEventListener("click", () => safeRun(() => runTest("telegram")));
    $("#test-imap-button").addEventListener("click", () => safeRun(() => runTest("imap")));
    $("#test-llm-button").addEventListener("click", () => safeRun(() => runTest("llm")));
    $("#create-demo-button").addEventListener("click", () => safeRun(() => createDemoForm(false)));
    $("#create-demo-auth-button").addEventListener("click", () => safeRun(() => createDemoForm(true)));
    $("#inspect-url-button").addEventListener("click", () => safeRun(inspectUrl));
    $("#create-from-url-button").addEventListener("click", () => safeRun(createFormFromUrl));
    $("#show-all-forms").addEventListener("change", () => safeRun(loadForms).then(renderForms));
    $("#save-fields-button").addEventListener("click", () => safeRun(saveFields));
    $("#remap-button").addEventListener("click", () => safeRun(remapFields));
    $("#validate-button").addEventListener("click", () => safeRun(validateFields));
    $("#fill-button").addEventListener("click", () => safeRun(fillForm));
    $("#submit-button").addEventListener("click", () => safeRun(submitForm));
    $("#manual-button").addEventListener("click", () => safeRun(manualForm));
    $("#cancel-button").addEventListener("click", () => safeRun(cancelForm));
    $("#reinspect-button").addEventListener("click", () => safeRun(reinspectForm));
    $("#open-original-button").addEventListener("click", () => {
      if (state.selectedForm?.form_url) window.open(state.selectedForm.form_url, "_blank", "noopener");
    });
    $("#save-profile-button").addEventListener("click", () => safeRun(saveProfile));
    $("#save-rules-button").addEventListener("click", () => safeRun(saveRules));
    $("#validate-rules-button").addEventListener("click", validateRulesJson);

    document.body.addEventListener("click", event => {
      const reviewButton = event.target.closest("[data-form-review]");
      if (reviewButton) safeRun(() => loadSelectedForm(reviewButton.dataset.formReview));
      const cancelButton = event.target.closest("[data-form-cancel]");
      if (cancelButton) {
        state.selectedFormId = cancelButton.dataset.formCancel;
        safeRun(cancelForm);
      }
      if (event.target.id === "auth-login-button") safeRun(authLogin);
      if (event.target.id === "auth-2fa-button") safeRun(submit2fa);
      if (event.target.id === "auth-reinspect-button") safeRun(reinspectForm);
      const explainButton = event.target.closest("[data-explain-field]");
      if (explainButton) safeRun(() => explainField(explainButton.dataset.explainField));
    });

    $("#manual-url").addEventListener("input", event => {
      $("#open-manual-url").href = event.target.value || "#";
    });
  }

  async function explainField(fieldId) {
    if (!state.selectedFormId) return;
    const data = await api(`/api/forms/${encodeURIComponent(state.selectedFormId)}/explain-field`, {
      method: "POST",
      body: JSON.stringify({ field_id: fieldId })
    });
    showResult("#test-result", data, "ok");
  }

  async function safeRun(fn) {
    try {
      return await fn();
    } catch (error) {
      showToast(error.message || String(error), "error");
      const currentSection = $(".section.is-active")?.id || "";
      if (currentSection === "section-manual") showResult("#manual-result", error.message, "error");
      else showResult("#test-result", error.message, "error");
      return null;
    }
  }

  function exposeGlobals() {
    window.createDemoForm = (...args) => safeRun(() => createDemoForm(...args));
    window.inspectUrl = () => safeRun(inspectUrl);
    window.createFormFromUrl = () => safeRun(createFormFromUrl);
    window.testBrowser = () => safeRun(() => runTest("browser"));
    window.testImap = () => safeRun(() => runTest("imap"));
    window.testLlm = () => safeRun(() => runTest("llm"));
    window.testTelegram = () => safeRun(() => runTest("telegram"));
    window.testAll = () => safeRun(testAll);
    window.remapFields = () => safeRun(remapFields);
    window.validateFields = () => safeRun(validateFields);
    window.fillForm = () => safeRun(fillForm);
    window.submitForm = () => safeRun(submitForm);
    window.cancelForm = () => safeRun(cancelForm);
    window.saveProfile = () => safeRun(saveProfile);
  }

  document.addEventListener("DOMContentLoaded", () => {
    bindEvents();
    exposeGlobals();
    safeRun(refreshAll);
    window.setInterval(() => safeRun(loadStatus).then(renderDashboard), 10000);
  });
})();
