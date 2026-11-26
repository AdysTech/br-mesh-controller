// JSON/HTTP helpers (see ApiHttp.h).
#include "ApiHttp.h"
#include <cstdio>
#include <cJSON.h>

/// Map an HTTP status code to the reason phrase httpd wants on the status line
/// (defaults to 200 OK for anything we don't special-case).
static const char *httpReason(int code) {
  switch (code) {
    case 200: return "200 OK";
    case 202: return "202 Accepted";
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default:  return "200 OK";
  }
}

void apiSendCJson(httpd_req_t *req, int httpCode, cJSON *obj) {
  char *s = obj ? cJSON_PrintUnformatted(obj) : nullptr;
  if (!s) {   // print failed (heap pressure) — minimal body, no recursion back
    if (obj) cJSON_Delete(obj);               // into apiSendError (would loop)
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"json build failed\"}", HTTPD_RESP_USE_STRLEN);
    return;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Expires", "0");
  httpd_resp_set_status(req, httpReason(httpCode ? httpCode : 200));
  httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
  cJSON_free(s);
  cJSON_Delete(obj);
}

void apiSendError(httpd_req_t *req, int httpCode, const char *msg) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return;
  cJSON_AddStringToObject(o, "error", msg ? msg : "error");
  apiSendCJson(req, httpCode, o);
}

String apiReadBody(httpd_req_t *req, size_t cap) {
  String body;
  char buf[256];
  for (;;) {
    int r = httpd_req_recv(req, buf, sizeof(buf));
    if (r <= 0) break;              // 0 = end of body / error
    body.concat(buf, r);            // recv does NOT null-terminate: append exactly r bytes
    if (body.length() >= cap) break;
  }
  if (body.length() > cap) body = body.substring(0, cap);
  return body;
}

std::string jsonEscape(const char *s) {
  std::string out;
  for (; s && *s; ++s) {
    const char c = *s;
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
          out += b;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Find "key" (as a JSON key) in the body; returns position just past the
// closing quote of the key, or npos.
static size_t jsonKeyPos(const String &body, const char *key) {
  return body.indexOf("\"" + String(key) + "\"");
}

int apiJsonInt(const String &body, const char *key) {
  size_t p = jsonKeyPos(body, key);
  if (p == -1) return -1;
  p = body.indexOf(':', p);
  if (p == -1) return -1;
  return atoi(body.substring(p + 1).c_str());
}

std::string apiJsonString(const String &body, const char *key) {
  size_t p = jsonKeyPos(body, key);
  if (p == -1) return "";
  p = body.indexOf(':', p);
  if (p == -1) return "";
  size_t q = body.indexOf('"', p + 1);
  if (q == -1) return "";
  size_t e = body.indexOf('"', q + 1);
  if (e == -1) return body.substring(q + 1).c_str(); // unterminated string — take the rest
  // substring(a, b) copies b-a bytes from a — the 2nd arg is an END index, not a
  // length (and it swaps the pair if a > b, which silently re-sliced the WRONG
  // region of the body). Use the end index e directly.
  return body.substring(q + 1, e).c_str();
}

bool apiJsonBool(const String &body, const char *key) {
  size_t p = jsonKeyPos(body, key);
  if (p == -1) return false;
  p = body.indexOf(':', p);
  if (p == -1) return false;
  return body.indexOf("true", p + 1) == p + 1 || body.indexOf("true", p + 2) == p + 2;
}

String apiQuery(httpd_req_t *req, const char *key) {
  static char qbuf[512];
  qbuf[0] = 0;
  // httpd_req_get_url_query_str returns esp_err_t (ESP_OK == 0), so test
  // against ESP_OK — NOT `!ret`, which inverts it (0 == success).
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) return String();
  // qbuf looks like "n=2&x=1"
  String q(qbuf);
  int start = 0;
  while (start <= q.length()) {
    int end = q.indexOf('&', start);
    if (end < 0) end = q.length();
    String kv = q.substring(start, end);
    int eq = kv.indexOf('=');
    if (eq > 0 && kv.substring(0, eq) == String(key))
      return kv.substring(eq + 1);
    start = end + 1;
  }
  return String();
}
