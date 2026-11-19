// JSON/HTTP helpers (see ApiHttp.h).
#include "ApiHttp.h"
#include <cstdio>

void apiSendJson(httpd_req_t *req, int httpCode, const char *jsonStr) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Expires", "0");
  httpd_resp_send(req, jsonStr, HTTPD_RESP_USE_STRLEN);
  (void)httpCode;  // httpd_resp_send sets 200; errors use apiSendError
}

void apiSendError(httpd_req_t *req, int httpCode, const char *msg) {
  char buf[192];
  char status[32];
  int n = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg ? msg : "error");
  snprintf(status, sizeof(status), "%d Error", httpCode ? httpCode : 500);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_status(req, status);
  httpd_resp_send(req, buf, n);
}

String apiReadBody(httpd_req_t *req, size_t cap) {
  String body;
  char buf[256];
  for (;;) {
    int r = httpd_req_recv(req, buf, sizeof(buf));
    if (r <= 0) break;              // 0 = end of body / error
    body += buf;
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

String apiQuery(httpd_req_t *req, const char *key) {
  static char qbuf[512];
  qbuf[0] = 0;
  if (!httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf))) return String();
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
