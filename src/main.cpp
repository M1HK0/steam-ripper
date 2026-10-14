// Local click UI for the Steam account that is logged in on this PC.
// Other saved accounts are never read and never changed.

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
static bool g_occluded = false;

struct Friend {
    std::string id;
    std::string name;
};

struct State {
    std::mutex mu;
    std::string status = "Uses only the account currently logged into the Steam client.";
    std::string persona;
    std::string steamId;
    std::string cookie;
    std::string session;
    std::vector<Friend> friends;
    int done = 0;
    int total = 0;
    bool busy = false;
    bool cancel = false;
};

static State g_state;
static std::thread g_job;

static bool IsSteamId(const std::string& s) {
    if (s.size() != 17 || s.compare(0, 7, "7656119") != 0)
        return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

static std::string UrlDecode(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return c - 'A' + 10;
            };
            out.push_back((char)((hex(s[i + 1]) << 4) | hex(s[i + 2])));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::string Unescape(std::string s) {
    auto rep = [&](const char* from, const char* to) {
        size_t p = 0;
        const size_t n = std::strlen(from);
        while ((p = s.find(from, p)) != std::string::npos) {
            s.replace(p, n, to);
            p += std::strlen(to);
        }
    };
    rep("&amp;", "&");
    rep("&lt;", "<");
    rep("&gt;", ">");
    rep("&quot;", "\"");
    rep("&#39;", "'");
    return s;
}

static std::string Trim(std::string s) {
    auto sp = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && sp((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && sp((unsigned char)s.back())) s.pop_back();
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);
    return s;
}

// Cookie header value: 7656...%7C%7C<jwt>. Rejects CR/LF so it cannot break the header.
static std::optional<std::string> NormalizeCookie(std::string raw) {
    raw = Trim(std::move(raw));
    const std::string key = "steamLoginSecure=";
    auto at = raw.find(key);
    if (at != std::string::npos)
        raw = raw.substr(at + key.size());
    auto semi = raw.find(';');
    if (semi != std::string::npos)
        raw = raw.substr(0, semi);
    raw = Trim(raw);
    std::string decoded = UrlDecode(raw);
    auto pipes = decoded.find("||");
    std::string value = pipes == std::string::npos ? raw : decoded.substr(0, pipes) + "%7C%7C" + decoded.substr(pipes + 2);
    if (value.empty() || value.find_first_of("\r\n; ") != std::string::npos)
        return std::nullopt;
    for (unsigned char c : value) {
        if (!(std::isalnum(c) || c == '%' || c == '.' || c == '-' || c == '_' || c == '~'))
            return std::nullopt;
    }
    return value;
}

static std::optional<std::string> SteamIdFromCookie(const std::string& cookie) {
    std::string decoded = UrlDecode(cookie);
    for (size_t i = 0; i + 17 <= decoded.size(); ++i) {
        std::string id = decoded.substr(i, 17);
        if (IsSteamId(id))
            return id;
    }
    return std::nullopt;
}

static bool RelationshipOk(const std::string& window) {
    auto num = window.find("efriendrelationship");
    if (num != std::string::npos) {
        auto colon = window.find(':', num);
        if (colon == std::string::npos || colon > num + 40)
            return false;
        size_t c = colon + 1;
        while (c < window.size() && std::isspace((unsigned char)window[c]))
            ++c;
        return c < window.size() && window[c] == '3' && (c + 1 == window.size() || !std::isdigit((unsigned char)window[c + 1]));
    }
    auto rel = window.find("\"relationship\"");
    if (rel == std::string::npos)
        return true;
    auto colon = window.find(':', rel);
    if (colon == std::string::npos)
        return false;
    auto q1 = window.find('"', colon + 1);
    if (q1 == std::string::npos)
        return false;
    auto q2 = window.find('"', q1 + 1);
    if (q2 == std::string::npos)
        return false;
    return window.substr(q1 + 1, q2 - q1 - 1) == "friend";
}

static std::vector<Friend> ParseJsonFriends(const std::string& body) {
    std::vector<Friend> out;
    std::unordered_set<std::string> seen;
    const std::string key = "ulfriendid";
    size_t p = 0;
    while ((p = body.find(key, p)) != std::string::npos) {
        auto colon = body.find(':', p + key.size());
        if (colon == std::string::npos || colon > p + 32) {
            p += key.size();
            continue;
        }
        auto q1 = body.find('"', colon);
        if (q1 == std::string::npos || q1 > colon + 8)
            break;
        auto q2 = body.find('"', q1 + 1);
        if (q2 == std::string::npos)
            break;
        std::string id = body.substr(q1 + 1, q2 - q1 - 1);
        size_t winStart = p > 400 ? p - 400 : 0;
        std::string window = body.substr(winStart, std::min(body.size(), q2 + 400) - winStart);
        p = q2 + 1;
        if (!IsSteamId(id) || !RelationshipOk(window) || !seen.insert(id).second)
            continue;
        out.push_back({id, {}});
    }
    return out;
}

static std::vector<Friend> ParseHtml(const std::string& body) {
    std::vector<Friend> out;
    std::unordered_set<std::string> seen;
    const std::string key = "data-steamid=\"";
    size_t p = 0;
    while ((p = body.find(key, p)) != std::string::npos) {
        size_t start = p + key.size();
        size_t end = body.find('"', start);
        if (end == std::string::npos)
            break;
        std::string id = body.substr(start, end - start);
        std::string name;
        size_t search = body.find("data-search=\"", end);
        if (search != std::string::npos && search < end + 240) {
            size_t ns = search + 13;
            size_t ne = body.find('"', ns);
            if (ne != std::string::npos) {
                name = body.substr(ns, ne - ns);
                auto semi = name.find(';');
                if (semi != std::string::npos)
                    name = name.substr(0, semi);
                name = Unescape(name);
            }
        }
        p = end + 1;
        if (!IsSteamId(id) || !seen.insert(id).second)
            continue;
        out.push_back({id, name});
    }
    return out;
}

static std::vector<Friend> ParseXml(const std::string& body) {
    std::vector<Friend> out;
    std::unordered_set<std::string> seen;
    const std::string open = "<steamid>";
    size_t p = 0;
    while ((p = body.find(open, p)) != std::string::npos) {
        size_t start = p + open.size();
        size_t end = body.find("</steamid>", start);
        if (end == std::string::npos)
            break;
        std::string id = body.substr(start, end - start);
        size_t blockStart = body.rfind("<friend>", start);
        size_t blockEnd = body.find("</friend>", start);
        bool keep = true;
        if (blockStart != std::string::npos && blockEnd != std::string::npos && blockStart < start && blockEnd > end) {
            std::string block = body.substr(blockStart, blockEnd - blockStart);
            auto rel = block.find("<relationship>");
            if (rel != std::string::npos) {
                auto relEnd = block.find("</relationship>", rel);
                if (relEnd == std::string::npos || block.substr(rel + 14, relEnd - (rel + 14)) != "friend")
                    keep = false;
            }
        }
        p = end + 11;
        if (!keep || !IsSteamId(id) || !seen.insert(id).second)
            continue;
        out.push_back({std::move(id), {}});
    }
    return out;
}

static std::vector<Friend> ParseFriends(const std::string& body) {
    if (body.find("ulfriendid") != std::string::npos) {
        auto parsed = ParseJsonFriends(body);
        if (!parsed.empty())
            return parsed;
    }
    if (body.find("data-steamid=\"") != std::string::npos) {
        auto parsed = ParseHtml(body);
        if (!parsed.empty())
            return parsed;
    }
    return ParseXml(body);
}

static std::string ParsePersona(const std::string& body) {
    const std::string open = "<steamID>";
    auto tag = body.find(open);
    if (tag == std::string::npos)
        return {};
    tag += open.size();
    auto end = body.find("</steamID>", tag);
    if (end == std::string::npos)
        return {};
    std::string s = body.substr(tag, end - tag);
    const std::string cdata = "<![CDATA[";
    if (s.compare(0, cdata.size(), cdata) == 0 && s.size() >= cdata.size() + 3 && s.compare(s.size() - 3, 3, "]]>") == 0)
        s = s.substr(cdata.size(), s.size() - cdata.size() - 3);
    return Unescape(s);
}

static std::string MakeSession() {
    unsigned char bytes[12]{};
    HCRYPTPROV prov = 0;
    if (CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(prov, sizeof(bytes), bytes);
        CryptReleaseContext(prov, 0);
    } else {
        DWORD tick = GetTickCount();
        for (size_t i = 0; i < sizeof(bytes); ++i)
            bytes[i] = (unsigned char)(tick + i * 17);
    }
    static const char* hex = "0123456789abcdef";
    std::string out(24, '0');
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0xf];
    }
    return out;
}

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
};

static std::wstring Widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

static bool ApplyLocation(const std::wstring& loc, std::wstring& host, std::wstring& path) {
    std::wstring url = loc;
    if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0) {
        auto rest = url.substr(url.find(L"://") + 3);
        auto slash = rest.find(L'/');
        host = slash == std::wstring::npos ? rest : rest.substr(0, slash);
        path = slash == std::wstring::npos ? L"/" : rest.substr(slash);
        auto colon = host.find(L':');
        if (colon != std::wstring::npos)
            host = host.substr(0, colon);
    } else if (!url.empty() && url[0] == L'/') {
        path = url;
    } else {
        return false;
    }
    if (host.find(L"steamcommunity.com") == std::wstring::npos && host.find(L"steampowered.com") == std::wstring::npos)
        return false;
    return true;
}

static HttpResponse Request(HINTERNET ses, const wchar_t* hostName, const wchar_t* method, std::wstring path, const std::string& cookie, const std::string& session,
                            const std::string& form, const std::wstring& referer) {
    HttpResponse resp;
    std::wstring host = hostName;
    bool post = std::wstring(method) == L"POST";
    for (int hop = 0; hop < 6; ++hop) {
        HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!con) {
            resp.error = "Could not connect (" + std::to_string(GetLastError()) + ").";
            return resp;
        }
        HINTERNET req = WinHttpOpenRequest(con, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) {
            WinHttpCloseHandle(con);
            resp.error = "Could not open request (" + std::to_string(GetLastError()) + ").";
            return resp;
        }
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

        std::wstring headers = L"Accept: */*\r\nCookie: sessionid=" + Widen(session);
        if (!cookie.empty())
            headers += L"; steamLoginSecure=" + Widen(cookie);
        headers += L"\r\n";
        if (post) {
            std::wstring origin = std::wstring(host) == L"store.steampowered.com" ? L"https://store.steampowered.com" : L"https://steamcommunity.com";
            headers += L"Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n";
            headers += L"X-Requested-With: XMLHttpRequest\r\n";
            headers += L"Origin: " + origin + L"\r\n";
            if (!referer.empty())
                headers += L"Referer: " + referer + L"\r\n";
        }

        BOOL sent = post
            ? WinHttpSendRequest(req, headers.c_str(), (DWORD)-1, (LPVOID)form.data(), (DWORD)form.size(), (DWORD)form.size(), 0)
            : WinHttpSendRequest(req, headers.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
            resp.error = "Request failed (" + std::to_string(GetLastError()) + ").";
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            return resp;
        }

        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        resp.status = (int)status;

        if (status >= 300 && status < 400) {
            wchar_t loc[2048]{};
            DWORD locLen = sizeof(loc);
            if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &locLen, WINHTTP_NO_HEADER_INDEX)) {
                resp.error = "Steam redirected without a location.";
                WinHttpCloseHandle(req);
                WinHttpCloseHandle(con);
                return resp;
            }
            std::wstring location = loc;
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            if (location.find(L"login") != std::wstring::npos) {
                resp.error = "Steam rejected the current login.";
                return resp;
            }
            if (post || !ApplyLocation(location, host, path)) {
                resp.error = "Steam redirected the request.";
                return resp;
            }
            method = L"GET";
            post = false;
            continue;
        }

        std::string body;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail))
                break;
            if (avail == 0)
                break;
            if (body.size() + avail > 8 * 1024 * 1024) {
                resp.error = "Steam's reply was too large.";
                break;
            }
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &read))
                break;
            chunk.resize(read);
            body += chunk;
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        resp.body = std::move(body);
        return resp;
    }
    resp.error = "Too many redirects.";
    return resp;
}

static void SetStatus(const std::string& text) {
    std::lock_guard lock(g_state.mu);
    g_state.status = text;
}

static bool Cancelled() {
    std::lock_guard lock(g_state.mu);
    return g_state.cancel;
}

static bool LoggedOut(const std::string& body) {
    return body.find("<title>Sign In") != std::string::npos || body.find("/login/home") != std::string::npos;
}

static std::string FormEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back((char)c);
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static uint32_t Crc32(const std::string& s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : s) {
        crc ^= c;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static std::string CacheKey(const std::string& accountName) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%08x1", Crc32(accountName));
    return buf;
}

static int B64Val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static std::string B64UrlDecode(std::string s) {
    while (s.size() % 4) s.push_back('=');
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        int digit = B64Val(c);
        if (digit < 0) break;
        val = (val << 6) + digit;
        bits += 6;
        if (bits >= 0) {
            out.push_back((char)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

static std::optional<std::string> JwtSub(const std::string& jwt) {
    auto a = jwt.find('.');
    if (a == std::string::npos) return std::nullopt;
    auto b = jwt.find('.', a + 1);
    if (b == std::string::npos) return std::nullopt;
    std::string payload = B64UrlDecode(jwt.substr(a + 1, b - a - 1));
    auto key = payload.find("\"sub\"");
    if (key == std::string::npos) return std::nullopt;
    auto colon = payload.find(':', key);
    if (colon == std::string::npos) return std::nullopt;
    auto q1 = payload.find('"', colon + 1);
    if (q1 == std::string::npos) return std::nullopt;
    auto q2 = payload.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::nullopt;
    return payload.substr(q1 + 1, q2 - q1 - 1);
}

static std::optional<std::string> JsonString(const std::string& body, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto at = body.find(needle);
    if (at == std::string::npos) return std::nullopt;
    auto colon = body.find(':', at + needle.size());
    if (colon == std::string::npos || colon > at + needle.size() + 8) return std::nullopt;
    auto q1 = body.find('"', colon + 1);
    if (q1 == std::string::npos || q1 > colon + 4) return std::nullopt;
    auto q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::nullopt;
    return body.substr(q1 + 1, q2 - q1 - 1);
}

static std::optional<std::string> BlockField(const std::string& text, const std::string& objectKey, const std::string& field) {
    std::string needle = "\"" + objectKey + "\"";
    size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        size_t i = at + needle.size();
        while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
        if (i >= text.size() || text[i] != '{') {
            at += needle.size();
            continue;
        }
        int depth = 0;
        size_t end = i;
        for (; end < text.size(); ++end) {
            if (text[end] == '{') ++depth;
            else if (text[end] == '}') {
                if (--depth == 0) { ++end; break; }
            }
        }
        std::string block = text.substr(i, end - i);
        std::string fneedle = "\"" + field + "\"";
        auto f = block.find(fneedle);
        if (f == std::string::npos) return std::nullopt;
        auto q1 = block.find('"', f + fneedle.size());
        if (q1 == std::string::npos) return std::nullopt;
        auto q2 = block.find('"', q1 + 1);
        if (q2 == std::string::npos) return std::nullopt;
        return block.substr(q1 + 1, q2 - q1 - 1);
    }
    return std::nullopt;
}

static std::optional<std::string> VdfQuoted(const std::string& text, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto at = text.find(needle);
    if (at == std::string::npos) return std::nullopt;
    auto q1 = text.find('"', at + needle.size());
    if (q1 == std::string::npos) return std::nullopt;
    auto q2 = text.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::nullopt;
    return text.substr(q1 + 1, q2 - q1 - 1);
}

static std::string ReadWhole(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string data;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(file, buf, sizeof(buf), &n, nullptr) && n)
        data.append(buf, n);
    CloseHandle(file);
    return data;
}

static std::wstring SteamPath() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buf[MAX_PATH]{};
    DWORD size = sizeof(buf), type = 0;
    if (RegQueryValueExW(key, L"SteamPath", nullptr, &type, (BYTE*)buf, &size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    std::wstring path = buf;
    for (wchar_t& c : path)
        if (c == L'/') c = L'\\';
    return path;
}

struct ActiveAccount {
    std::string steamId;
    std::string accountName;
    std::string persona;
    std::string error;
};

static std::optional<std::string> ActiveSteamId() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return std::nullopt;
    DWORD user = 0, pid = 0, size = sizeof(DWORD), type = 0;
    RegQueryValueExW(key, L"ActiveUser", nullptr, &type, (BYTE*)&user, &size);
    size = sizeof(DWORD);
    RegQueryValueExW(key, L"pid", nullptr, &type, (BYTE*)&pid, &size);
    RegCloseKey(key);
    if (user == 0 || pid == 0) return std::nullopt;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return std::nullopt;
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    if (code != STILL_ACTIVE) return std::nullopt;
    return std::to_string(76561197960265728ull + user);
}

static ActiveAccount CurrentAccount() {
    ActiveAccount account;
    auto steamId = ActiveSteamId();
    if (!steamId) {
        account.error = "Steam is not logged in. Open the Steam client and log in. Other accounts are not used.";
        return account;
    }
    std::wstring users = SteamPath() + L"\\config\\loginusers.vdf";
    std::string text = ReadWhole(users);
    auto name = BlockField(text, *steamId, "AccountName");
    if (!name || name->empty()) {
        account.error = "Could not read the logged-in account. Other accounts were not used.";
        return account;
    }
    account.steamId = *steamId;
    account.accountName = *name;
    if (auto persona = BlockField(text, *steamId, "PersonaName"))
        account.persona = *persona;
    return account;
}

static bool SameAccount(const std::string& steamId) {
    auto live = ActiveSteamId();
    return live && *live == steamId;
}

static std::vector<unsigned char> HexToBytes(const std::string& hex) {
    std::vector<unsigned char> out;
    if (hex.size() % 2) return out;
    out.reserve(hex.size() / 2);
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nyb(hex[i]), lo = nyb(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back((unsigned char)((hi << 4) | lo));
    }
    return out;
}

static std::optional<std::string> DecryptToken(const std::vector<unsigned char>& blob, const std::string& accountName) {
    DATA_BLOB in{};
    in.cbData = (DWORD)blob.size();
    in.pbData = const_cast<BYTE*>(blob.data());
    DATA_BLOB entropy{};
    entropy.cbData = (DWORD)accountName.size();
    entropy.pbData = (BYTE*)accountName.data();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    std::string token((char*)out.pbData, (char*)out.pbData + out.cbData);
    LocalFree(out.pbData);
    while (!token.empty() && (token.back() == '\0' || std::isspace((unsigned char)token.back())))
        token.pop_back();
    return token;
}

struct Login {
    std::string steamId;
    std::string persona;
    std::string cookie;
    std::string session;
    std::string accessToken;
    std::string error;
};

static Login OpenSession(HINTERNET ses) {
    Login login;
    ActiveAccount account = CurrentAccount();
    if (!account.error.empty()) {
        login.error = account.error;
        return login;
    }
    wchar_t localApp[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH)) {
        login.error = "Could not find the Steam login for this Windows user.";
        return login;
    }
    std::string local = ReadWhole(std::wstring(localApp) + L"\\Steam\\local.vdf");
    auto hex = VdfQuoted(local, CacheKey(account.accountName));
    if (!hex) {
        login.error = "No saved login for the account currently in Steam. Other accounts were not used.";
        return login;
    }
    auto blob = HexToBytes(*hex);
    auto refresh = blob.empty() ? std::nullopt : DecryptToken(blob, account.accountName);
    auto refreshSub = refresh ? JwtSub(*refresh) : std::nullopt;
    if (!refresh || !refreshSub || *refreshSub != account.steamId) {
        login.error = "The saved login does not match the account in Steam. Stopped. Nothing was changed.";
        return login;
    }

    login.session = MakeSession();
    login.steamId = account.steamId;
    login.persona = account.persona;
    std::string token = *refresh;
    std::string form = "refresh_token=" + FormEncode(*refresh) + "&steamid=" + account.steamId;
    auto minted = Request(ses, L"api.steampowered.com", L"POST", L"/IAuthenticationService/GenerateAccessTokenForApp/v1/", "", login.session, form, L"https://steamcommunity.com/");
    if (auto access = JsonString(minted.body, "access_token")) {
        auto sub = JwtSub(*access);
        if (sub && *sub == account.steamId)
            token = *access;
    }
    if (auto sub = JwtSub(token); !sub || *sub != account.steamId) {
        login.error = "Steam returned a login for a different account. Stopped. Nothing was changed.";
        return login;
    }
    login.accessToken = token;
    login.cookie = account.steamId + "%7C%7C" + token;
    if (!SameAccount(account.steamId)) {
        login.error = "The Steam login changed. Stopped. No other account was touched.";
        return login;
    }
    auto me = Request(ses, L"steamcommunity.com", L"GET", L"/my/?xml=1", login.cookie, login.session, {}, L"");
    if (!me.error.empty()) {
        login.error = me.error;
        return login;
    }
    if (me.status != 200 || me.body.find(account.steamId) == std::string::npos || LoggedOut(me.body)) {
        login.error = "Steam did not accept the current login. Stay logged into the Steam client and try again.";
        return login;
    }
    if (auto persona = ParsePersona(me.body); !persona.empty())
        login.persona = persona;
    return login;
}

static std::vector<std::string> ParseFreePackages(const std::string& html) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    const std::string key = "RemoveFreeLicense(";
    size_t p = 0;
    while ((p = html.find(key, p)) != std::string::npos) {
        p += key.size();
        while (p < html.size() && std::isspace((unsigned char)html[p])) ++p;
        size_t start = p;
        while (p < html.size() && std::isdigit((unsigned char)html[p])) ++p;
        if (p == start) continue;
        std::string id = html.substr(start, p - start);
        if (id != "0" && seen.insert(id).second)
            out.push_back(std::move(id));
    }
    return out;
}

struct FamilyInfo {
    bool member = false;
    std::string groupId;
};

static FamilyInfo ParseFamily(const std::string& body) {
    FamilyInfo info;
    if (body.find("\"is_not_member_of_any_group\":true") != std::string::npos || body.find("\"is_not_member_of_any_group\": true") != std::string::npos)
        return info;
    auto key = body.find("\"family_groupid\"");
    if (key == std::string::npos) return info;
    auto colon = body.find(':', key + 16);
    if (colon == std::string::npos || colon > key + 40) return info;
    size_t i = colon + 1;
    while (i < body.size() && std::isspace((unsigned char)body[i])) ++i;
    if (i < body.size() && body[i] == '"') ++i;
    size_t start = i;
    while (i < body.size() && std::isdigit((unsigned char)body[i])) ++i;
    if (i == start) return info;
    info.groupId = body.substr(start, i - start);
    info.member = info.groupId != "0";
    return info;
}

static std::string Snip(std::string s) {
    if (s.find("eyJ") != std::string::npos) return "Steam returned an error.";
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (s.size() > 160) s.resize(160);
    return s;
}

static void LoadJob() {
    struct Finish {
        ~Finish() {
            std::lock_guard lock(g_state.mu);
            g_state.busy = false;
        }
    } finish;

    HINTERNET ses = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        SetStatus("Could not start HTTP.");
        return;
    }
    WinHttpSetTimeouts(ses, 8000, 8000, 20000, 20000);
    struct Ses {
        HINTERNET h;
        ~Ses() { if (h) WinHttpCloseHandle(h); }
    } sesGuard{ses};

    Login login = OpenSession(ses);
    if (!login.error.empty()) {
        SetStatus(login.error);
        return;
    }
    if (Cancelled()) {
        SetStatus("Stopped.");
        return;
    }

    std::wstring friendsPath = L"/profiles/" + Widen(login.steamId) + L"/friends?xml=1";
    auto page = Request(ses, L"steamcommunity.com", L"GET", friendsPath, login.cookie, login.session, {}, L"");
    if (!page.error.empty()) {
        SetStatus(page.error);
        return;
    }

    std::vector<Friend> friends;
    bool xmlList = page.body.find("<friendsList") != std::string::npos;
    if (xmlList)
        friends = ParseXml(page.body);
    else
        friends = ParseFriends(page.body);

    if (!xmlList && friends.empty()) {
        auto html = Request(ses, L"steamcommunity.com", L"GET", L"/profiles/" + Widen(login.steamId) + L"/friends/", login.cookie, login.session, {}, L"");
        if (!html.error.empty()) {
            SetStatus(html.error);
            return;
        }
        if (LoggedOut(html.body)) {
            SetStatus("Steam did not accept the current login.");
            return;
        }
        friends = ParseFriends(html.body);
    }

    std::erase_if(friends, [&](const Friend& f) { return f.id == login.steamId; });

    {
        std::lock_guard lock(g_state.mu);
        g_state.cookie = login.cookie;
        g_state.session = login.session;
        g_state.steamId = login.steamId;
        g_state.persona = login.persona;
        g_state.friends = std::move(friends);
        g_state.done = 0;
        g_state.total = (int)g_state.friends.size();
        if (g_state.persona.empty())
            g_state.status = "Loaded " + g_state.steamId + ". " + std::to_string(g_state.total) + " friends.";
        else
            g_state.status = "Loaded " + g_state.persona + ". " + std::to_string(g_state.total) + " friends.";
    }
}

static void RemoveJob() {
    struct Finish {
        ~Finish() {
            std::lock_guard lock(g_state.mu);
            g_state.busy = false;
        }
    } finish;

    std::string cookie, session, steamId;
    std::vector<Friend> friends;
    {
        std::lock_guard lock(g_state.mu);
        cookie = g_state.cookie;
        session = g_state.session;
        steamId = g_state.steamId;
        friends = g_state.friends;
        g_state.done = 0;
        g_state.total = (int)friends.size();
    }
    if (cookie.empty() || friends.empty() || !IsSteamId(steamId)) {
        SetStatus("Load friends first.");
        return;
    }
    if (!SameAccount(steamId)) {
        SetStatus("The Steam login changed. Stopped. No other account was touched.");
        return;
    }

    HINTERNET ses = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        SetStatus("Could not start HTTP.");
        return;
    }
    WinHttpSetTimeouts(ses, 8000, 8000, 20000, 20000);
    struct Ses {
        HINTERNET h;
        ~Ses() { if (h) WinHttpCloseHandle(h); }
    } sesGuard{ses};

    std::wstring referer = L"https://steamcommunity.com/profiles/" + Widen(steamId) + L"/friends/";
    int removed = 0;
    for (size_t i = 0; i < friends.size(); ++i) {
        if (Cancelled() || !SameAccount(steamId)) {
            std::lock_guard lock(g_state.mu);
            g_state.status = SameAccount(steamId)
                ? "Stopped after removing " + std::to_string(removed) + ". " + std::to_string(g_state.friends.size()) + " still listed."
                : "The Steam login changed. Stopped. No other account was touched.";
            return;
        }
        const std::string& id = friends[i].id;
        std::string form = "sessionID=" + session + "&sessionid=" + session + "&steamid=" + id;
        auto res = Request(ses, L"steamcommunity.com", L"POST", L"/actions/RemoveFriendAjax", cookie, session, form, referer);
        bool badBody = res.body.find("\"success\":false") != std::string::npos || res.body.find("\"success\": false") != std::string::npos;
        if (!res.error.empty() || res.status != 200 || badBody) {
            std::lock_guard lock(g_state.mu);
            g_state.status = "Stopped at " + id + " after removing " + std::to_string(removed) + ". " +
                             (res.error.empty() ? ("Steam returned " + std::to_string(res.status) + ".") : res.error);
            return;
        }
        ++removed;
        {
            std::lock_guard lock(g_state.mu);
            if (!g_state.friends.empty() && g_state.friends.front().id == id)
                g_state.friends.erase(g_state.friends.begin());
            g_state.done = removed;
            g_state.status = "Removing " + std::to_string(removed) + " / " + std::to_string((int)friends.size());
        }
        // ponytail: one remove every 300ms. Fine for a few hundred; raise it only after seeing Steam's real limit.
        for (int t = 0; t < 6 && !Cancelled(); ++t)
            Sleep(50);
    }
    std::lock_guard lock(g_state.mu);
    g_state.status = "Removed " + std::to_string(removed) + " friends from " + (g_state.persona.empty() ? g_state.steamId : g_state.persona) + ".";
}

static void RemoveFreeJob() {
    struct Finish {
        ~Finish() {
            std::lock_guard lock(g_state.mu);
            g_state.busy = false;
        }
    } finish;

    HINTERNET ses = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        SetStatus("Could not start HTTP.");
        return;
    }
    WinHttpSetTimeouts(ses, 8000, 8000, 20000, 20000);
    struct Ses {
        HINTERNET h;
        ~Ses() { if (h) WinHttpCloseHandle(h); }
    } sesGuard{ses};

    Login login = OpenSession(ses);
    if (!login.error.empty()) {
        SetStatus(login.error);
        return;
    }
    auto page = Request(ses, L"store.steampowered.com", L"GET", L"/account/licenses/?l=english", login.cookie, login.session, {}, L"https://store.steampowered.com/account/");
    if (!page.error.empty()) {
        SetStatus(page.error);
        return;
    }
    if (page.status != 200 || LoggedOut(page.body)) {
        SetStatus("Steam did not open the library page for the logged-in account.");
        return;
    }
    auto packages = ParseFreePackages(page.body);
    if (packages.empty()) {
        SetStatus("No free games with a remove button were found. Paid games were not touched.");
        return;
    }
    {
        std::lock_guard lock(g_state.mu);
        g_state.done = 0;
        g_state.total = (int)packages.size();
        g_state.steamId = login.steamId;
        g_state.persona = login.persona;
    }
    int removed = 0;
    for (const auto& packageId : packages) {
        if (Cancelled() || !SameAccount(login.steamId)) {
            SetStatus(!SameAccount(login.steamId)
                ? "The Steam login changed. Stopped. No other account was touched."
                : "Stopped after removing " + std::to_string(removed) + " free games.");
            return;
        }
        std::string form = "sessionid=" + login.session + "&packageid=" + packageId;
        auto res = Request(ses, L"store.steampowered.com", L"POST", L"/account/removelicense", login.cookie, login.session, form, L"https://store.steampowered.com/account/licenses/");
        bool ok = res.error.empty() && res.status == 200 &&
                  (res.body.find("\"success\":1") != std::string::npos || res.body.find("\"success\": 1") != std::string::npos);
        if (!ok) {
            SetStatus("Stopped at package " + packageId + " after removing " + std::to_string(removed) + " free games. " +
                      (res.error.empty() ? Snip(res.body.empty() ? "Steam returned " + std::to_string(res.status) + "." : res.body) : res.error));
            return;
        }
        ++removed;
        {
            std::lock_guard lock(g_state.mu);
            g_state.done = removed;
            g_state.status = "Removing free games " + std::to_string(removed) + " / " + std::to_string((int)packages.size());
        }
        // ponytail: one license every 300ms. The store caps requests; speed up only after measuring that cap.
        for (int t = 0; t < 6 && !Cancelled(); ++t)
            Sleep(50);
    }
    SetStatus("Removed " + std::to_string(removed) + " free games from " + (login.persona.empty() ? login.steamId : login.persona) + ". Paid games were not touched.");
}

static void RenameJob(std::string name) {
    struct Finish {
        ~Finish() {
            std::lock_guard lock(g_state.mu);
            g_state.busy = false;
        }
    } finish;

    while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(name.begin());
    while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
    if (name.empty() || name.size() > 64) {
        SetStatus("Enter a profile name, up to 64 characters.");
        return;
    }

    HINTERNET ses = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        SetStatus("Could not start HTTP.");
        return;
    }
    WinHttpSetTimeouts(ses, 8000, 8000, 20000, 20000);
    struct Ses {
        HINTERNET h;
        ~Ses() { if (h) WinHttpCloseHandle(h); }
    } sesGuard{ses};

    Login login = OpenSession(ses);
    if (!login.error.empty()) {
        SetStatus(login.error);
        return;
    }
    if (!SameAccount(login.steamId)) {
        SetStatus("The Steam login changed. The name was not changed.");
        return;
    }
    std::string form = "sessionid=" + login.session + "&personaName=" + FormEncode(name);
    std::wstring path = L"/profiles/" + Widen(login.steamId) + L"/ajaxsetpersonaname/";
    auto res = Request(ses, L"steamcommunity.com", L"POST", path, login.cookie, login.session, form, L"https://steamcommunity.com/profiles/" + Widen(login.steamId) + L"/edit/info");
    bool ok = res.error.empty() && res.status == 200 && res.body.find("\"success\":false") == std::string::npos && res.body.find("\"success\":0") == std::string::npos;
    if (!ok) {
        SetStatus(res.error.empty() ? "Name was not changed. " + Snip(res.body.empty() ? "Steam returned " + std::to_string(res.status) + "." : res.body) : res.error);
        return;
    }
    {
        std::lock_guard lock(g_state.mu);
        g_state.persona = name;
        g_state.steamId = login.steamId;
    }
    SetStatus("Profile name set to " + name + ".");
}

static void LeaveFamilyJob() {
    struct Finish {
        ~Finish() {
            std::lock_guard lock(g_state.mu);
            g_state.busy = false;
        }
    } finish;

    HINTERNET ses = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        SetStatus("Could not start HTTP.");
        return;
    }
    WinHttpSetTimeouts(ses, 8000, 8000, 20000, 20000);
    struct Ses {
        HINTERNET h;
        ~Ses() { if (h) WinHttpCloseHandle(h); }
    } sesGuard{ses};

    Login login = OpenSession(ses);
    if (!login.error.empty()) {
        SetStatus(login.error);
        return;
    }
    if (!SameAccount(login.steamId)) {
        SetStatus("The Steam login changed. Stopped. No other account was touched.");
        return;
    }
    std::string lookup = "access_token=" + FormEncode(login.accessToken) + "&input_json=" + FormEncode("{}");
    auto found = Request(ses, L"api.steampowered.com", L"POST", L"/IFamilyGroupsService/GetFamilyGroupForUser/v1/", "", login.session, lookup, L"https://steamcommunity.com/");
    if (!found.error.empty() || found.status != 200) {
        SetStatus(found.error.empty() ? "Could not read the Steam family. " + Snip(found.body) : found.error);
        return;
    }
    FamilyInfo family = ParseFamily(found.body);
    if (!family.member) {
        SetStatus("This account is not in a Steam family.");
        return;
    }
    // steamid_to_remove is always the logged-in account. This leaves; it does not remove anyone else.
    std::string input = "{\"family_groupid\":\"" + family.groupId + "\",\"steamid_to_remove\":\"" + login.steamId + "\"}";
    std::string form = "access_token=" + FormEncode(login.accessToken) + "&input_json=" + FormEncode(input);
    auto left = Request(ses, L"api.steampowered.com", L"POST", L"/IFamilyGroupsService/RemoveFromFamilyGroup/v1/", "", login.session, form, L"https://steamcommunity.com/");
    if (!left.error.empty() || left.status != 200 || left.body.find("\"error\"") != std::string::npos) {
        SetStatus("Steam did not let this account leave. If you are the only adult, closing the family would remove the other members, and this button does not do that. " +
                  (left.error.empty() ? Snip(left.body) : left.error));
        return;
    }
    SetStatus("Left the Steam family on " + (login.persona.empty() ? login.steamId : login.persona) + ".");
}

static bool TryStart() {
    std::lock_guard lock(g_state.mu);
    if (g_state.busy)
        return false;
    g_state.busy = true;
    g_state.cancel = false;
    return true;
}

static int SelfCheck() {
    auto fail = [](const char* msg) {
        std::printf("fail %s\n", msg);
        return 1;
    };
    auto cookie = NormalizeCookie("  steamLoginSecure=76561198000000000%7C%7CeyJhbGciOi.abc-_; Path=/  ");
    if (!cookie || *cookie != "76561198000000000%7C%7CeyJhbGciOi.abc-_")
        return fail("normalize encoded");
    auto decoded = NormalizeCookie("76561198000000001||eyJ");
    if (!decoded || *decoded != "76561198000000001%7C%7CeyJ")
        return fail("normalize pipes");
    if (NormalizeCookie("76561198000000000%7C%7Cbad\r\ninjected"))
        return fail("reject crlf");
    auto id = SteamIdFromCookie(*cookie);
    if (!id || *id != "76561198000000000")
        return fail("steamid");

    const char* xml =
        "<friendsList><friend><steamid>76561198000000002</steamid><relationship>friend</relationship></friend>"
        "<friend><steamid>76561198000000003</steamid><relationship>requestrecipient</relationship></friend>"
        "<friend><steamid>76561198000000004</steamid></friend></friendsList>"
        "<steamID64>76561198000000009</steamID64>";
    auto parsedXml = ParseFriends(xml);
    if (parsedXml.size() != 2 || parsedXml[0].id != "76561198000000002" || parsedXml[1].id != "76561198000000004")
        return fail("xml");

    const char* html =
        "<div data-steamid=\"76561198000000005\" data-search=\"a&amp;b;alias\">"
        "<div data-steamid=\"76561198000000005\" data-search=\"dup\">";
    auto parsedHtml = ParseFriends(html);
    if (parsedHtml.size() != 1 || parsedHtml[0].name != "a&b")
        return fail("html");

    const char* json =
        "{\"response\":{\"friends\":["
        "{\"ulfriendid\":\"76561198000000006\",\"efriendrelationship\":3},"
        "{\"efriendrelationship\":2,\"ulfriendid\":\"76561198000000007\"}"
        "]}}";
    auto parsedJson = ParseFriends(json);
    if (parsedJson.size() != 1 || parsedJson[0].id != "76561198000000006")
        return fail("json");

    if (ParsePersona("<steamID><![CDATA[Star &amp; co]]></steamID><steamID64>76561198000000000</steamID64>") != "Star & co")
        return fail("persona");
    if (!ParseFriends("<steamID64>76561198000000009</steamID64>").empty())
        return fail("profile id is not a friend");
    if (Crc32("123456789") != 0xCBF43926u || CacheKey("123456789") != "cbf439261")
        return fail("crc");
    if (B64UrlDecode("aGk") != "hi")
        return fail("b64");
    const char* users =
        "\"users\" {\n"
        "\"76561198000000000\" {\n"
        "\"AccountName\" \"onlythis\"\n"
        "\"PersonaName\" \"Me\"\n"
        "}\n"
        "\"76561198000000001\" {\n"
        "\"AccountName\" \"other\"\n"
        "\"PersonaName\" \"Nope\"\n"
        "}\n}";
    auto only = BlockField(users, "76561198000000000", "AccountName");
    auto other = BlockField(users, "76561198000000001", "AccountName");
    if (!only || *only != "onlythis" || !other || *other != "other")
        return fail("account block");
    if (BlockField(users, "76561198000000000", "Missing"))
        return fail("missing field");
    const char* licenses = "paid RemoveLicense(9) <a onclick=\"RemoveFreeLicense( 42 )\">x</a> RemoveFreeLicense(42) RemoveFreeLicense(7)";
    auto freePkgs = ParseFreePackages(licenses);
    if (freePkgs.size() != 2 || freePkgs[0] != "42" || freePkgs[1] != "7")
        return fail("free packages");
    auto inFamily = ParseFamily("{\"response\":{\"family_groupid\":\"99\",\"is_not_member_of_any_group\":false}}");
    auto noFamily = ParseFamily("{\"response\":{\"is_not_member_of_any_group\":true,\"latest_joined_family_groupid\":\"99\"}}");
    if (!inFamily.member || inFamily.groupId != "99" || noFamily.member)
        return fail("family");
    if (FormEncode("a b&c") != "a%20b%26c")
        return fail("form");
    std::printf("self-check ok\n");
    return 0;
}

static bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (hr != S_OK)
        return false;
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    return true;
}

static void Resize(UINT w, UINT h) {
    if (!g_swap)
        return;
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_resizeW = LOWORD(lp);
            g_resizeH = HIWORD(lp);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int RunGui() {
    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"SteamFriends", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Steam account", WS_OVERLAPPEDWINDOW, 100, 60, (int)(540 * scale), (int)(780 * scale), nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDevice(hwnd)) {
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.WindowRounding = 0;
    style.FrameRounding = 4;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 1.f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.14f, 1.f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.18f, 0.34f, 0.52f, 1.f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.44f, 0.66f, 1.f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.30f, 0.46f, 1.f);

    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    static char profileName[65] = {};
    bool confirm = false;

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_occluded = false;
        if (g_resizeW && g_resizeH) {
            Resize(g_resizeW, g_resizeH);
            g_resizeW = g_resizeH = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        std::string status, persona, steamId;
        std::vector<Friend> friends;
        int progressDone = 0, progressTotal = 0;
        bool busy = false;
        {
            std::lock_guard lock(g_state.mu);
            status = g_state.status;
            persona = g_state.persona;
            steamId = g_state.steamId;
            friends = g_state.friends;
            progressDone = g_state.done;
            progressTotal = g_state.total;
            busy = g_state.busy;
        }

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("Steam account", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        ActiveAccount live = CurrentAccount();
        bool loggedIn = live.error.empty();
        ImGui::TextUnformatted("Only the account logged into the Steam client");
        std::string shownPersona = live.persona;
        if (loggedIn && steamId == live.steamId && !persona.empty())
            shownPersona = persona;
        if (!loggedIn)
            ImGui::TextWrapped("%s", live.error.c_str());
        else if (shownPersona.empty())
            ImGui::Text("Logged in: %s", live.steamId.c_str());
        else
            ImGui::Text("Logged in: %s (%s)", shownPersona.c_str(), live.steamId.c_str());
        ImGui::TextWrapped("Other saved accounts are not used.");
        ImGui::Spacing();

        ImGui::BeginDisabled(busy || !loggedIn);
        if (ImGui::Button("Load friends", ImVec2(-1, 0)) && TryStart()) {
            confirm = false;
            {
                std::lock_guard lock(g_state.mu);
                g_state.friends.clear();
                g_state.persona.clear();
                g_state.steamId.clear();
                g_state.cookie.clear();
                g_state.done = 0;
                g_state.total = 0;
                g_state.status = "Loading friends...";
            }
            if (g_job.joinable())
                g_job.join();
            g_job = std::thread(LoadJob);
        }
        ImGui::EndDisabled();

        ImGui::Text("%d friends", (int)friends.size());
        ImGui::BeginChild("list", ImVec2(0, -230), ImGuiChildFlags_Borders);
        for (const auto& f : friends) {
            if (f.name.empty())
                ImGui::TextUnformatted(f.id.c_str());
            else
                ImGui::Text("%s    %s", f.name.c_str(), f.id.c_str());
        }
        ImGui::EndChild();

        bool friendsMatch = loggedIn && !steamId.empty() && steamId == live.steamId;
        ImGui::BeginDisabled(busy || !friendsMatch);
        if (ImGui::Checkbox("Remove every friend on this logged-in account", &confirm)) {
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(busy || !confirm || !friendsMatch);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.16f, 0.16f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.22f, 0.22f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.12f, 0.12f, 1.f));
        if (ImGui::Button("Remove all friends", ImVec2(-1, 32)) && TryStart()) {
            if (g_job.joinable())
                g_job.join();
            g_job = std::thread(RemoveJob);
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::BeginDisabled(busy || !loggedIn);
        if (ImGui::Button("Remove all free games", ImVec2(-1, 0)) && TryStart()) {
            {
                std::lock_guard lock(g_state.mu);
                g_state.done = 0;
                g_state.total = 0;
                g_state.status = "Looking up free games...";
            }
            if (g_job.joinable())
                g_job.join();
            g_job = std::thread(RemoveFreeJob);
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110);
        ImGui::InputTextWithHint("##name", "New profile name", profileName, sizeof(profileName));
        ImGui::SameLine();
        if (ImGui::Button("Set name", ImVec2(-1, 0)) && TryStart()) {
            std::string wanted = profileName;
            {
                std::lock_guard lock(g_state.mu);
                g_state.status = "Setting profile name...";
            }
            if (g_job.joinable())
                g_job.join();
            g_job = std::thread(RenameJob, std::move(wanted));
        }
        if (ImGui::Button("Leave Steam family", ImVec2(-1, 0)) && TryStart()) {
            {
                std::lock_guard lock(g_state.mu);
                g_state.status = "Leaving Steam family...";
            }
            if (g_job.joinable())
                g_job.join();
            g_job = std::thread(LeaveFamilyJob);
        }
        ImGui::EndDisabled();

        if (busy && ImGui::Button("Stop", ImVec2(-1, 0))) {
            std::lock_guard lock(g_state.mu);
            g_state.cancel = true;
            g_state.status = "Stopping...";
        }

        if (progressTotal > 0 && (busy || progressDone > 0))
            ImGui::ProgressBar(progressTotal ? (float)progressDone / (float)progressTotal : 0.f, ImVec2(-1, 0));
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::End();

        ImGui::Render();
        const float clear[] = {0.08f, 0.09f, 0.11f, 1.f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = (g_swap->Present(1, 0) == DXGI_STATUS_OCCLUDED);
    }

    {
        std::lock_guard lock(g_state.mu);
        g_state.cancel = true;
    }
    if (g_job.joinable())
        g_job.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-check") == 0) {
            AttachConsole(ATTACH_PARENT_PROCESS);
            FILE* out = nullptr;
            freopen_s(&out, "CONOUT$", "w", stdout);
            return SelfCheck();
        }
    }
    return RunGui();
}
