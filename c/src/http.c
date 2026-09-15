/*
 * HTTPS GET over WinHTTP -- task 10 of the port-to-c-sas4-cli plan.
 */
#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMEOUT_MS 30000 /* urlopen(..., timeout=30) */

static void fail(char *err, size_t err_len, const char *what) {
    if (err && err_len) snprintf(err, err_len, "%s (WinHTTP error %lu)", what, GetLastError());
}

/* UTF-8 -> UTF-16 into a malloc'd buffer, or NULL. */
static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

bool http_get(const char *url, unsigned char **out, size_t *out_len, char *err,
              size_t err_len) {
    *out = NULL;
    *out_len = 0;

    wchar_t *wurl = widen(url);
    if (!wurl) {
        if (err && err_len) snprintf(err, err_len, "cannot encode the URL");
        return false;
    }

    /* WinHttpCrackUrl writes back pointers INTO wurl plus lengths, so the components are not
     * NUL-terminated; copy each one out before use. */
    wchar_t host[256], path[2048];
    URL_COMPONENTS parts;
    memset(&parts, 0, sizeof parts);
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;
    parts.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &parts)) {
        fail(err, err_len, "cannot parse the URL");
        free(wurl);
        return false;
    }
    if (parts.dwHostNameLength >= sizeof host / sizeof *host
        || (size_t)parts.dwUrlPathLength + parts.dwExtraInfoLength
               >= sizeof path / sizeof *path) {
        if (err && err_len) snprintf(err, err_len, "the URL is too long");
        free(wurl);
        return false;
    }
    memcpy(host, parts.lpszHostName, parts.dwHostNameLength * sizeof *host);
    host[parts.dwHostNameLength] = L'\0';
    memcpy(path, parts.lpszUrlPath, parts.dwUrlPathLength * sizeof *path);
    memcpy(path + parts.dwUrlPathLength, parts.lpszExtraInfo,
           parts.dwExtraInfoLength * sizeof *path);
    path[parts.dwUrlPathLength + parts.dwExtraInfoLength] = L'\0';
    bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    INTERNET_PORT port = parts.nPort;
    free(wurl);

    HINTERNET session = WinHttpOpen(L"sas4", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        fail(err, err_len, "cannot start WinHTTP");
        return false;
    }
    WinHttpSetTimeouts(session, TIMEOUT_MS, TIMEOUT_MS, TIMEOUT_MS, TIMEOUT_MS);

    bool ok = false;
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    HINTERNET conn = WinHttpConnect(session, host, port, 0);
    HINTERNET req = NULL;
    if (!conn) {
        fail(err, err_len, "cannot reach the host");
        goto done;
    }
    req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        fail(err, err_len, "cannot open the request");
        goto done;
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0)) {
        fail(err, err_len, "cannot send the request");
        goto done;
    }
    if (!WinHttpReceiveResponse(req, NULL)) {
        fail(err, err_len, "no response");
        goto done;
    }

    DWORD status = 0, status_len = sizeof status;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
                             WINHTTP_NO_HEADER_INDEX)) {
        fail(err, err_len, "no status line");
        goto done;
    }
    if (status < 200 || status > 299) {
        if (err && err_len) snprintf(err, err_len, "HTTP %lu", (unsigned long)status);
        goto done;
    }

    /* The response has no length we can trust, so grow as it arrives. */
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            fail(err, err_len, "the transfer failed");
            goto done;
        }
        if (avail == 0) break;
        if (len + avail + 1 > cap) {
            size_t want = cap ? cap * 2 : 65536;
            while (want < len + avail + 1) want *= 2;
            unsigned char *bigger = realloc(buf, want);
            if (!bigger) {
                if (err && err_len) snprintf(err, err_len, "out of memory");
                goto done;
            }
            buf = bigger;
            cap = want;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf + len, avail, &got)) {
            fail(err, err_len, "the transfer failed");
            goto done;
        }
        if (got == 0) break;
        len += got;
    }
    if (!buf) { /* a zero-byte body still has to come back as a valid buffer */
        buf = malloc(1);
        if (!buf) {
            if (err && err_len) snprintf(err, err_len, "out of memory");
            goto done;
        }
    }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    buf = NULL;
    ok = true;

done:
    free(buf);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}
