#pragma once
// Small JSON/HTTP helpers for the esp_http_server handlers.
#include <Arduino.h>
#include <esp_http_server.h>
#include <string>

/// Send a JSON string with Content-Type + cache-busting headers.
void apiSendJson(httpd_req_t *req, int httpCode, const char *jsonStr);

/// Send a JSON error object {"error": "msg"}.
void apiSendError(httpd_req_t *req, int httpCode, const char *msg);

/// Read the request body into a String (capped; caller uses small bodies).
String apiReadBody(httpd_req_t *req, size_t cap = 4096);

/// Get a URL query parameter value, or "" if absent.
String apiQuery(httpd_req_t *req, const char *key);

/// Escape a C string for embedding inside a JSON string literal
/// (quotes, backslashes, control chars).
std::string jsonEscape(const char *s);

// --- tiny flat-JSON body readers (no JSON lib on this board) ----------------
/// Extract an integer "key":<num> from a small JSON body; -1 when absent.
int apiJsonInt(const String &body, const char *key);

/// Extract a string "key":"<text>" from a small JSON body; "" when absent.
std::string apiJsonString(const String &body, const char *key);

/// Extract a boolean "key":true|false from a small JSON body; false when
/// absent (or not true).
bool apiJsonBool(const String &body, const char *key);
