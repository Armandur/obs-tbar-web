/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "tbar-web.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <obs-data.h>

#ifdef ENABLE_FRONTEND_API
#include <obs-frontend-api.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* OBS frontend T-bar range is integer 0..1023 */
#define TBAR_MAX 1023
#define TBAR_CLAMP 10

static ULONGLONG g_last_release_tick = 0;
static bool g_manual_active = false;
static ULONGLONG g_last_start_tick = 0;
static obs_source_t *g_manual_program = NULL;
static obs_source_t *g_manual_preview = NULL;

static void manual_clear_state(void)
{
	if (g_manual_program) {
		obs_source_release(g_manual_program);
		g_manual_program = NULL;
	}
	if (g_manual_preview) {
		obs_source_release(g_manual_preview);
		g_manual_preview = NULL;
	}
	g_manual_active = false;
}

/* ------------------------------ */
/* Minimal HTTP server (Windows)  */
/* ------------------------------ */

#ifdef _WIN32

static struct {
	bool running;
	volatile bool stop;
	HANDLE thread;
	SOCKET listen_sock;
	int port;
	double last_position; /* last position we applied via POST */
} g_srv = {0};

static struct {
	bool enabled;
	int port;
} g_cfg = {
	.enabled = true,
	.port = 4455,
};

static void cfg_set_defaults(obs_data_t *data)
{
	obs_data_set_default_bool(data, "enabled", true);
	obs_data_set_default_int(data, "port", 4455);
}

static const char *cfg_path(void)
{
	return obs_module_config_path("obs-tbar-web.json");
}

static void cfg_load(void)
{
	const char *path = cfg_path();
	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	if (!data)
		data = obs_data_create();

	cfg_set_defaults(data);

	g_cfg.enabled = obs_data_get_bool(data, "enabled");
	g_cfg.port = (int)obs_data_get_int(data, "port");
	if (g_cfg.port <= 0 || g_cfg.port > 65535)
		g_cfg.port = 4455;

	obs_data_release(data);
}

static void cfg_save(void)
{
	const char *path = cfg_path();
	obs_data_t *data = obs_data_create();
	cfg_set_defaults(data);
	obs_data_set_bool(data, "enabled", g_cfg.enabled);
	obs_data_set_int(data, "port", g_cfg.port);
	obs_data_save_json_pretty_safe(data, path, "tmp", "bak");
	obs_data_release(data);
}

static void cfg_apply(void)
{
	if (!g_cfg.enabled) {
		tbar_web_stop();
		obs_log(LOG_INFO, "tbar-web: disabled via config");
		return;
	}

	/* Restart if port changed */
	if (g_srv.running && g_srv.port != g_cfg.port) {
		tbar_web_stop();
	}
	tbar_web_start(g_cfg.port);
}

static void cfg_apply_task(void *unused)
{
	(void)unused;
	cfg_apply();
}

static void http_send(SOCKET s, int code, const char *status, const char *content_type, const char *body)
{
	if (!content_type)
		content_type = "text/plain; charset=utf-8";
	if (!body)
		body = "";

	char headers[512];
	int body_len = (int)strlen(body);
	int n = snprintf(headers, sizeof(headers),
			 "HTTP/1.1 %d %s\r\n"
			 "Content-Type: %s\r\n"
			 "Content-Length: %d\r\n"
			 "Connection: close\r\n"
			 "Access-Control-Allow-Origin: *\r\n"
			 "Access-Control-Allow-Headers: Content-Type\r\n"
			 "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
			 "\r\n",
			 code, status, content_type, body_len);
	if (n > 0) {
		send(s, headers, n, 0);
	}
	if (body_len > 0) {
		send(s, body, body_len, 0);
	}
}

static int str_case_starts_with(const char *s, const char *prefix)
{
	while (*prefix && *s) {
		if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
			return 0;
		s++;
		prefix++;
	}
	return *prefix == '\0';
}

static const char *find_header_value(const char *headers, const char *key)
{
	/* Very small header parser: find "Key:" at line start */
	const char *p = headers;
	size_t key_len = strlen(key);

	while (*p) {
		const char *line = p;
		const char *eol = strstr(line, "\r\n");
		if (!eol)
			break;

		if ((size_t)(eol - line) > key_len + 1) {
			if (str_case_starts_with(line, key) && line[key_len] == ':') {
				const char *v = line + key_len + 1;
				while (*v == ' ' || *v == '\t')
					v++;
				return v;
			}
		}

		p = eol + 2;
	}
	return NULL;
}

static bool parse_json_position(const char *body, double *out_pos)
{
	/* Minimal and forgiving: search for "position" and parse following number */
	if (!body || !out_pos)
		return false;

	const char *p = strstr(body, "position");
	if (!p)
		return false;

	/* find ':' */
	p = strchr(p, ':');
	if (!p)
		return false;
	p++;
	while (*p && isspace((unsigned char)*p))
		p++;

	char *end = NULL;
	double v = strtod(p, &end);
	if (end == p)
		return false;
	/* Accept either normalized [0..1] or integer [0..1023].
	   Keep backward compat with older [0..10000] scaling if someone used it. */
	if (v > 1.0 && v <= (double)TBAR_MAX)
		v = v / (double)TBAR_MAX;
	else if (v > 1.0 && v <= 10000.0)
		v = v / 10000.0;
	if (v < 0.0)
		v = 0.0;
	if (v > 1.0)
		v = 1.0;
	*out_pos = v;
	return true;
}

static bool parse_json_release(const char *body, bool *out_release)
{
	if (!body || !out_release)
		return false;

	const char *p = strstr(body, "release");
	if (!p)
		return false;

	p = strchr(p, ':');
	if (!p)
		return false;
	p++;
	while (*p && isspace((unsigned char)*p))
		p++;

	if (str_case_starts_with(p, "true")) {
		*out_release = true;
		return true;
	}
	if (str_case_starts_with(p, "false")) {
		*out_release = false;
		return true;
	}

	return false;
}

struct set_pos_task_data {
	double pos;
	bool release;
};

static void set_pos_task(void *param)
{
	struct set_pos_task_data *d = param;
#ifdef ENABLE_FRONTEND_API
	/* Clamp */
	double t = d->pos;
	if (t < 0.0)
		t = 0.0;
	if (t > 1.0)
		t = 1.0;
	g_srv.last_position = t;

	/* Only meaningful in Studio Mode */
	if (!obs_frontend_preview_program_mode_active()) {
		obs_log(LOG_INFO, "tbar-web: ignored (not in Studio Mode)");
		free(d);
		return;
	}

	obs_source_t *transition = obs_get_output_source(0);
	if (!transition) {
		obs_log(LOG_WARNING, "tbar-web: no transition output source");
		free(d);
		return;
	}

	/* Some transitions (e.g. Cut) are fixed/instant and can't be driven manually. */
	const bool fixed = obs_transition_fixed(transition);

	/* Start a manual transition the first time we move away from 0. */
	if (!g_manual_active && t > 0.0f) {
		ULONGLONG now = GetTickCount64();
		if (now - g_last_start_tick > 250) {
			g_last_start_tick = now;

			manual_clear_state();
			g_manual_program = obs_frontend_get_current_scene();
			g_manual_preview = obs_frontend_get_current_preview_scene();

			if (!g_manual_program || !g_manual_preview) {
				obs_log(LOG_WARNING, "tbar-web: missing program/preview scene");
				manual_clear_state();
			} else if (g_manual_program == g_manual_preview) {
				obs_log(LOG_INFO, "tbar-web: program == preview (nothing to transition)");
				manual_clear_state();
			} else if (fixed) {
				/* For fixed transitions, we don't start manual. We'll trigger on release at max. */
				obs_log(LOG_INFO, "tbar-web: current transition is fixed; manual tbar disabled (will trigger on release)");
				manual_clear_state();
			} else {
				/* duration_ms is required; manual mode uses manual_time for progress */
				uint32_t dur = (uint32_t)obs_frontend_get_transition_duration();
				if (dur < 50)
					dur = 300;

				bool ok = obs_transition_start(transition, OBS_TRANSITION_MODE_MANUAL, dur, g_manual_preview);
				if (ok) {
					g_manual_active = true;
					obs_log(LOG_INFO, "tbar-web: manual transition started");
				} else {
					obs_log(LOG_WARNING, "tbar-web: failed to start manual transition");
					manual_clear_state();
				}
			}
		}
	}

	/* Drive the transition */
	if (g_manual_active)
		obs_transition_set_manual_time(transition, (float)t);

	/* Optional release: finish (near 1) or cancel (near 0) and reset state */
	if (d->release) {
		ULONGLONG now = GetTickCount64();
		if (now - g_last_release_tick < 250) {
			obs_log(LOG_INFO, "tbar-web: release ignored (debounce)");
		} else {
			g_last_release_tick = now;

			const double t_finish = (double)(TBAR_MAX - TBAR_CLAMP) / (double)TBAR_MAX;
			const double t_cancel = (double)TBAR_CLAMP / (double)TBAR_MAX;

			if (fixed && t >= t_finish) {
				/* Cut/etc: do an actual program transition via frontend */
				obs_frontend_preview_program_trigger_transition();
				obs_log(LOG_INFO, "tbar-web: fixed transition trigger");
				g_srv.last_position = 0.0;
				manual_clear_state();
			} else if (g_manual_active && t >= t_finish) {
				obs_transition_set_manual_time(transition, 1.0f);
				/* Commit swap in Studio Mode: program becomes old preview; preview becomes old program */
				if (g_manual_preview && g_manual_program) {
					obs_frontend_set_current_scene(g_manual_preview);
					obs_frontend_set_current_preview_scene(g_manual_program);
				}
				obs_log(LOG_INFO, "tbar-web: manual transition finish+swap");
				g_srv.last_position = 0.0;
				manual_clear_state();
			} else if (g_manual_active && t <= t_cancel) {
				obs_transition_set_manual_time(transition, 0.0f);
				obs_transition_force_stop(transition);
				obs_log(LOG_INFO, "tbar-web: manual transition cancel");
				g_srv.last_position = 0.0;
				manual_clear_state();
			}
		}
	}

	obs_source_release(transition);
#else
	(void)d;
#endif
	free(d);
}

static void handle_request(SOCKET s, const char *req, const char *body, int body_len)
{
	(void)body_len;

	char method[16] = {0};
	char path[256] = {0};

	if (sscanf(req, "%15s %255s", method, path) != 2) {
		http_send(s, 400, "Bad Request", "text/plain; charset=utf-8", "bad request");
		return;
	}

	if (strcmp(method, "OPTIONS") == 0) {
		http_send(s, 204, "No Content", NULL, "");
		return;
	}

	if (strcmp(path, "/favicon.ico") == 0) {
		http_send(s, 204, "No Content", NULL, "");
		return;
	}

	if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
		const char *html =
			"<!doctype html>\n"
			"<html lang=\"en\">\n"
			"<head>\n"
			"  <meta charset=\"utf-8\" />\n"
			"  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />\n"
			"  <title>OBS T-bar test</title>\n"
			"  <style>\n"
			"    :root { color-scheme: dark; }\n"
			"    body { margin: 0; font-family: system-ui, -apple-system, Segoe UI, Roboto, Arial; background:#0b0f14; color:#e8eef7; }\n"
			"    .wrap { max-width: 760px; margin: 40px auto; padding: 0 16px; }\n"
			"    .card { background:#121a24; border:1px solid #223247; border-radius: 14px; padding: 18px; }\n"
			"    h1 { font-size: 18px; margin: 0 0 12px; }\n"
			"    .row { display:flex; align-items:center; gap: 12px; }\n"
			"    input[type=range] { width: 100%; }\n"
			"    .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; opacity: .9; }\n"
			"    .muted { opacity: .75; font-size: 13px; margin-top: 10px; }\n"
			"    .ok { color:#6ee7b7; }\n"
			"    .bad { color:#fca5a5; }\n"
			"    button { background:#1b2a3d; color:#e8eef7; border:1px solid #2c425f; border-radius: 10px; padding: 8px 10px; cursor:pointer; }\n"
			"    button:hover { background:#22324a; }\n"
			"    label { user-select: none; }\n"
			"    input[type=number] { width: 110px; background:#0b0f14; color:#e8eef7; border:1px solid #2c425f; border-radius: 10px; padding: 6px 8px; }\n"
			"    input[type=checkbox] { transform: scale(1.1); }\n"
			"    .hr { height:1px; background:#223247; margin: 14px 0; }\n"
			"  </style>\n"
			"</head>\n"
			"<body>\n"
			"  <div class=\"wrap\">\n"
			"    <div class=\"card\">\n"
			"      <h1>OBS T-bar test</h1>\n"
			"      <div class=\"row\">\n"
			"        <input id=\"slider\" type=\"range\" min=\"0\" max=\"1023\" step=\"1\" value=\"0\" />\n"
			"        <div class=\"mono\" style=\"min-width: 120px; text-align:right;\">\n"
			"          <div><span id=\"pct\">0.0</span>%</div>\n"
			"          <div style=\"opacity:.7\">(<span id=\"raw\">0</span>)</div>\n"
			"        </div>\n"
			"      </div>\n"
			"      <div class=\"row\" style=\"margin-top: 12px; justify-content: space-between;\">\n"
			"        <div class=\"mono\">Status: <span id=\"status\" class=\"muted\">—</span></div>\n"
			"        <div class=\"row\">\n"
			"          <button id=\"btn0\" type=\"button\">0%</button>\n"
			"          <button id=\"btn50\" type=\"button\">50%</button>\n"
			"          <button id=\"btn100\" type=\"button\">100%</button>\n"
			"        </div>\n"
			"      </div>\n"
			"      <div class=\"muted\">\n"
			"        Tip: keep OBS in Studio Mode while testing. This page sends POST <span class=\"mono\">/tbar</span>.\n"
			"      </div>\n"
			"      <div class=\"hr\"></div>\n"
			"      <div class=\"row\" style=\"justify-content: space-between; align-items: flex-start; gap: 16px;\">\n"
			"        <div>\n"
			"          <div class=\"mono\" style=\"margin-bottom: 6px;\">Settings</div>\n"
			"          <div class=\"row\" style=\"gap:10px; flex-wrap: wrap;\">\n"
			"            <label class=\"row\" style=\"gap:8px;\"><input id=\"cfgEnabled\" type=\"checkbox\" /> enabled</label>\n"
			"            <label class=\"row\" style=\"gap:8px;\">port <input id=\"cfgPort\" type=\"number\" min=\"1\" max=\"65535\" /></label>\n"
			"            <button id=\"cfgSave\" type=\"button\">Save</button>\n"
			"          </div>\n"
			"          <div class=\"muted\" id=\"cfgHint\"></div>\n"
			"        </div>\n"
			"        <div class=\"mono\" style=\"text-align:right; opacity:.8;\">\n"
			"          <div>GET <span class=\"mono\">/config</span></div>\n"
			"          <div>POST <span class=\"mono\">/config</span></div>\n"
			"        </div>\n"
			"      </div>\n"
			"    </div>\n"
			"  </div>\n"
			"\n"
			"  <script>\n"
			"    const slider = document.getElementById('slider');\n"
			"    const pct = document.getElementById('pct');\n"
			"    const raw = document.getElementById('raw');\n"
			"    const status = document.getElementById('status');\n"
			"    const cfgEnabled = document.getElementById('cfgEnabled');\n"
			"    const cfgPort = document.getElementById('cfgPort');\n"
			"    const cfgSave = document.getElementById('cfgSave');\n"
			"    const cfgHint = document.getElementById('cfgHint');\n"
			"\n"
			"    function setUi(v) {\n"
			"      const n = Number(v);\n"
			"      raw.textContent = String(n);\n"
			"      pct.textContent = (n / 1023 * 100).toFixed(1);\n"
			"    }\n"
			"\n"
			"    function setStatus(ok, text) {\n"
			"      status.textContent = text;\n"
			"      status.className = ok ? 'ok mono' : 'bad mono';\n"
			"    }\n"
			"\n"
			"    function setCfgHint(ok, text) {\n"
			"      cfgHint.textContent = text || '';\n"
			"      cfgHint.className = ok ? 'muted ok' : 'muted bad';\n"
			"    }\n"
			"\n"
			"    let inflight = false;\n"
			"    let pending = null;\n"
			"    let paused = false;\n"
			"    let released = false;\n"
			"    let releaseInFlight = false;\n"
			"\n"
			"    async function waitForIdle() {\n"
			"      while (inflight) {\n"
			"        await new Promise(r => setTimeout(r, 10));\n"
			"      }\n"
			"    }\n"
			"\n"
			"    async function send(v) {\n"
			"      if (paused) return;\n"
			"      pending = v;\n"
			"      if (inflight) return;\n"
			"      inflight = true;\n"
			"      while (pending !== null && !paused) {\n"
			"        const cur = pending;\n"
			"        pending = null;\n"
			"        try {\n"
			"          const position = cur / 1023;\n"
			"          const r = await fetch('/tbar', {\n"
			"            method: 'POST',\n"
			"            headers: { 'Content-Type': 'application/json' },\n"
			"            body: JSON.stringify({ position })\n"
			"          });\n"
			"          if (!r.ok) throw new Error('HTTP ' + r.status);\n"
			"          setStatus(true, 'OK');\n"
			"        } catch (e) {\n"
			"          setStatus(false, String(e));\n"
			"        }\n"
			"      }\n"
			"      inflight = false;\n"
			"    }\n"
			"\n"
			"    async function releaseIfAtMax() {\n"
			"      const v = Number(slider.value);\n"
			"      const max = Number(slider.max);\n"
			"      const clamp = 10;\n"
			"      if (v < (max - clamp)) return;\n"
			"      if (released || releaseInFlight) return;\n"
			"      try {\n"
			"        releaseInFlight = true;\n"
			"        // Stop sending new position updates; wait for the last in-flight POST to finish.\n"
			"        paused = true;\n"
			"        pending = null;\n"
			"        await waitForIdle();\n"
			"        const r = await fetch('/tbar', {\n"
			"          method: 'POST',\n"
			"          headers: { 'Content-Type': 'application/json' },\n"
			"          body: JSON.stringify({ position: 1.0, release: true })\n"
			"        });\n"
			"        if (!r.ok) throw new Error('HTTP ' + r.status);\n"
			"        released = true;\n"
			"        // Reset UI, then after a short delay reset OBS tbar to 0 so next run starts clean.\n"
			"        slider.value = 0;\n"
			"        setUi(0);\n"
			"        setStatus(true, 'release');\n"
			"      } catch (e) {\n"
			"        setStatus(false, 'release: ' + String(e));\n"
			"      } finally {\n"
			"        releaseInFlight = false;\n"
			"        paused = false;\n"
			"      }\n"
			"    }\n"
			"\n"
			"    slider.addEventListener('input', () => {\n"
			"      const v = Number(slider.value);\n"
			"      if (v < (Number(slider.max) - 10)) released = false;\n"
			"      setUi(v);\n"
			"      send(v);\n"
			"    });\n"
			"\n"
			"    slider.addEventListener('pointerup', releaseIfAtMax);\n"
			"\n"
			"    document.getElementById('btn0').onclick = () => { slider.value = 0; slider.dispatchEvent(new Event('input')); };\n"
			"    document.getElementById('btn50').onclick = () => { slider.value = 512; slider.dispatchEvent(new Event('input')); };\n"
			"    document.getElementById('btn100').onclick = () => { slider.value = 1023; slider.dispatchEvent(new Event('input')); releaseIfAtMax(); };\n"
			"\n"
			"    (async function init() {\n"
			"      try {\n"
			"        const rc = await fetch('/config');\n"
			"        if (rc.ok) {\n"
			"          const c = await rc.json();\n"
			"          cfgEnabled.checked = !!c.enabled;\n"
			"          cfgPort.value = String(c.port || 4455);\n"
			"          setCfgHint(true, '');\n"
			"        }\n"
			"        cfgSave.onclick = async () => {\n"
			"          try {\n"
			"            const newPort = Number(cfgPort.value);\n"
			"            const payload = { enabled: !!cfgEnabled.checked, port: newPort };\n"
			"            const r = await fetch('/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });\n"
			"            if (!r.ok) throw new Error('HTTP ' + r.status);\n"
			"            const j = await r.json();\n"
			"            if (j.port && j.port != location.port) {\n"
			"              setCfgHint(true, 'Port changed. Open: http://127.0.0.1:' + j.port + '/');\n"
			"            } else {\n"
			"              setCfgHint(true, 'Saved');\n"
			"            }\n"
			"          } catch (e) {\n"
			"            setCfgHint(false, String(e));\n"
			"          }\n"
			"        };\n"
			"\n"
			"        const r = await fetch('/tbar');\n"
			"        if (r.ok) {\n"
			"          const j = await r.json();\n"
			"          const v = Math.round((Number(j.position) || 0) * 1023);\n"
			"          slider.value = v;\n"
			"          setUi(v);\n"
			"          setStatus(true, 'ready');\n"
			"        }\n"
			"      } catch (_) {}\n"
			"    })();\n"
			"  </script>\n"
			"</body>\n"
			"</html>\n";

		http_send(s, 200, "OK", "text/html; charset=utf-8", html);
		return;
	}

	if (strcmp(path, "/config") == 0) {
		if (strcmp(method, "GET") == 0) {
			char resp[128];
			snprintf(resp, sizeof(resp), "{\"enabled\":%s,\"port\":%d}",
				 g_cfg.enabled ? "true" : "false", g_cfg.port);
			http_send(s, 200, "OK", "application/json; charset=utf-8", resp);
			return;
		}

		if (strcmp(method, "POST") == 0) {
			/* Minimal parsing: { "enabled": true/false, "port": 4455 } */
			bool enabled = g_cfg.enabled;
			int port = g_cfg.port;

			const char *p_enabled = strstr(body, "enabled");
			if (p_enabled) {
				const char *c = strchr(p_enabled, ':');
				if (c) {
					c++;
					while (*c && isspace((unsigned char)*c))
						c++;
					if (str_case_starts_with(c, "true"))
						enabled = true;
					else if (str_case_starts_with(c, "false"))
						enabled = false;
				}
			}

			const char *p_port = strstr(body, "port");
			if (p_port) {
				const char *c = strchr(p_port, ':');
				if (c) {
					c++;
					while (*c && isspace((unsigned char)*c))
						c++;
					int v = atoi(c);
					if (v > 0 && v <= 65535)
						port = v;
				}
			}

			g_cfg.enabled = enabled;
			g_cfg.port = port;
			cfg_save();

			/* Apply asynchronously; we can't stop/restart server on the server thread. */
			obs_queue_task(OBS_TASK_UI, cfg_apply_task, NULL, false);

			char resp[128];
			snprintf(resp, sizeof(resp), "{\"ok\":true,\"enabled\":%s,\"port\":%d}",
				 g_cfg.enabled ? "true" : "false", g_cfg.port);
			http_send(s, 200, "OK", "application/json; charset=utf-8", resp);
			return;
		}

		http_send(s, 405, "Method Not Allowed", "application/json; charset=utf-8",
			  "{\"error\":\"method_not_allowed\"}");
		return;
	}

	if (strcmp(path, "/status") == 0) {
		if (strcmp(method, "GET") == 0) {
			char resp[256];
			snprintf(resp, sizeof(resp),
				 "{\"ok\":true,\"enabled\":%s,\"port\":%d,\"manual_active\":%s,\"last_position\":%.6f}",
				 g_cfg.enabled ? "true" : "false", g_cfg.port, g_manual_active ? "true" : "false",
				 g_srv.last_position);
			http_send(s, 200, "OK", "application/json; charset=utf-8", resp);
			return;
		}
		http_send(s, 405, "Method Not Allowed", "application/json; charset=utf-8",
			  "{\"error\":\"method_not_allowed\"}");
		return;
	}

	if (strcmp(path, "/tbar") != 0) {
		http_send(s, 404, "Not Found", "application/json; charset=utf-8",
			  "{\"error\":\"not_found\"}");
		return;
	}

	if (strcmp(method, "GET") == 0) {
		char resp[128];
		/* We currently report the last position we applied via POST.
		   (We can later add true readback if we find a get API or a signal.) */
		snprintf(resp, sizeof(resp), "{\"position\":%.6f,\"source\":\"cached\"}", g_srv.last_position);
		http_send(s, 200, "OK", "application/json; charset=utf-8", resp);
		return;
	}

	if (strcmp(method, "POST") == 0) {
		double pos = 0.0;
		if (!parse_json_position(body, &pos)) {
			http_send(s, 400, "Bad Request", "application/json; charset=utf-8",
				  "{\"error\":\"invalid_json\"}");
			return;
		}

		g_srv.last_position = pos;

		struct set_pos_task_data *d = malloc(sizeof(*d));
		if (!d) {
			http_send(s, 500, "Internal Server Error", "application/json; charset=utf-8",
				  "{\"error\":\"oom\"}");
			return;
		}
		d->pos = pos;
		d->release = false;
		(void)parse_json_release(body, &d->release);
		/* Always execute on UI task queue to keep frontend calls off the socket thread. */
		obs_queue_task(OBS_TASK_UI, set_pos_task, d, false);

		http_send(s, 200, "OK", "application/json; charset=utf-8",
			  "{\"ok\":true}");
		return;
	}

	http_send(s, 405, "Method Not Allowed", "application/json; charset=utf-8",
		  "{\"error\":\"method_not_allowed\"}");
}

static unsigned __stdcall server_thread(void *unused)
{
	(void)unused;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		obs_log(LOG_ERROR, "tbar-web: WSAStartup failed");
		return 0;
	}

	g_srv.listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_srv.listen_sock == INVALID_SOCKET) {
		obs_log(LOG_ERROR, "tbar-web: socket() failed");
		WSACleanup();
		return 0;
	}

	BOOL opt = TRUE;
	setsockopt(g_srv.listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)g_srv.port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if (bind(g_srv.listen_sock, (struct sockaddr *)&addr, (int)sizeof(addr)) != 0) {
		obs_log(LOG_ERROR, "tbar-web: bind(127.0.0.1:%d) failed", g_srv.port);
		closesocket(g_srv.listen_sock);
		g_srv.listen_sock = INVALID_SOCKET;
		WSACleanup();
		return 0;
	}

	if (listen(g_srv.listen_sock, SOMAXCONN) != 0) {
		obs_log(LOG_ERROR, "tbar-web: listen() failed");
		closesocket(g_srv.listen_sock);
		g_srv.listen_sock = INVALID_SOCKET;
		WSACleanup();
		return 0;
	}

	obs_log(LOG_INFO, "tbar-web: listening on http://127.0.0.1:%d", g_srv.port);

	while (!g_srv.stop) {
		SOCKET client = accept(g_srv.listen_sock, NULL, NULL);
		if (client == INVALID_SOCKET) {
			/* likely closed during shutdown */
			break;
		}

		char buf[8192];
		int got = recv(client, buf, (int)sizeof(buf) - 1, 0);
		if (got <= 0) {
			closesocket(client);
			continue;
		}
		buf[got] = '\0';

		const char *header_end = strstr(buf, "\r\n\r\n");
		const char *body = "";
		if (header_end) {
			body = header_end + 4;
		}

		/* Best effort: if Content-Length exists and body incomplete, read more. */
		int content_len = 0;
		const char *cl = find_header_value(buf, "Content-Length");
		if (cl) {
			content_len = atoi(cl);
		}

		/* If declared content length larger than what we already have, read remaining (up to buffer). */
		int have_body = (int)strlen(body);
		if (content_len > have_body && header_end) {
			int remaining = content_len - have_body;
			int space = (int)sizeof(buf) - 1 - got;
			if (remaining > 0 && space > 0) {
				int to_read = remaining < space ? remaining : space;
				int got2 = recv(client, buf + got, to_read, 0);
				if (got2 > 0) {
					got += got2;
					buf[got] = '\0';
					header_end = strstr(buf, "\r\n\r\n");
					body = header_end ? header_end + 4 : "";
				}
			}
		}

		handle_request(client, buf, body, content_len);
		closesocket(client);
	}

	if (g_srv.listen_sock != INVALID_SOCKET) {
		closesocket(g_srv.listen_sock);
		g_srv.listen_sock = INVALID_SOCKET;
	}
	WSACleanup();

	obs_log(LOG_INFO, "tbar-web: stopped");
	return 0;
}

bool tbar_web_start(int port)
{
	if (g_srv.running)
		return true;

	if (port <= 0 || port > 65535)
		port = 4455;

	g_srv.port = port;
	g_srv.stop = false;
	g_srv.listen_sock = INVALID_SOCKET;
	g_srv.last_position = 0.0;

	uintptr_t th = _beginthreadex(NULL, 0, server_thread, NULL, 0, NULL);
	if (th == 0) {
		obs_log(LOG_ERROR, "tbar-web: failed to start thread (_beginthreadex)");
		return false;
	}

	g_srv.thread = (HANDLE)th;
	g_srv.running = true;
	return true;
}

void tbar_web_stop(void)
{
	if (!g_srv.running)
		return;

	g_srv.stop = true;
	/* Force accept() to wake up */
	if (g_srv.listen_sock != INVALID_SOCKET) {
		closesocket(g_srv.listen_sock);
		g_srv.listen_sock = INVALID_SOCKET;
	}

	WaitForSingleObject(g_srv.thread, INFINITE);
	CloseHandle(g_srv.thread);
	g_srv.thread = NULL;
	g_srv.running = false;
}

void tbar_web_apply_config(void)
{
	cfg_load();
	cfg_apply();
}

#else /* _WIN32 */

bool tbar_web_start(int port)
{
	(void)port;
	obs_log(LOG_WARNING, "tbar-web: HTTP server not implemented on this platform yet");
	return false;
}

void tbar_web_stop(void) {}

void tbar_web_apply_config(void) {}

#endif

