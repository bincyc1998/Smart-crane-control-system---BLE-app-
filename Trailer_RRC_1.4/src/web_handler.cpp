#include "web_handler.h"

#define MAX_SESSIONS 5
struct WebSession {
  String token;
  String role;       // "Admin" / "Operator"
  String userEmail;  // empty = web admin, else BLE user email
};
static WebSession sessions[MAX_SESSIONS];
static int sessionNext = 0; // round-robin eviction counter


// Web admin credentials (mutable, persisted in NVS namespace "webadmin")
static char webAdminUser[64] = "webadmin@plc.com";
static char webAdminPass[64] = "WebAdmin123";

static void loadWebAdmin() {
  prefs.begin("webadmin", true);
  String u = prefs.getString("user", "webadmin@plc.com");
  String p = prefs.getString("pass", "WebAdmin123");
  prefs.end();
  // Migrate stale NVS that stored a plain username (no @) — reset to email default
  if (u.indexOf('@') < 0) {
    u = "webadmin@plc.com";
    p = "WebAdmin123";
    prefs.begin("webadmin", false);
    prefs.putString("user", u);
    prefs.putString("pass", p);
    prefs.end();
    Serial.println("Web admin migrated to email login (webadmin@plc.com / WebAdmin123)");
  }
  u.toCharArray(webAdminUser, sizeof(webAdminUser));
  p.toCharArray(webAdminPass, sizeof(webAdminPass));
}

static void saveWebAdmin() {
  prefs.begin("webadmin", false);
  prefs.putString("user", webAdminUser);
  prefs.putString("pass", webAdminPass);
  prefs.end();
}

static String generateToken() {
  char buf[33];
  for (int i = 0; i < 32; i++) {
    buf[i] = "0123456789abcdef"[random(16)];
  }
  buf[32] = '\0';
  return String(buf);
}

static String getSessionCookie() {
  String cookieHeader = webServer.header("Cookie");
  int idx = cookieHeader.indexOf("session=");
  if (idx < 0) return "";
  int start = idx + 8;
  int end = cookieHeader.indexOf(';', start);
  if (end < 0) return cookieHeader.substring(start);
  return cookieHeader.substring(start, end);
}

// Find session slot for the current request cookie; returns -1 if not found
static int findSessionIdx() {
  String cookie = getSessionCookie();
  if (cookie.length() == 0) return -1;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token.length() > 0 && sessions[i].token == cookie) return i;
  }
  return -1;
}
// Helpers: role and email for the current request
static String currentRole()      { int i = findSessionIdx(); return i >= 0 ? sessions[i].role      : ""; }
static String currentUserEmail() { int i = findSessionIdx(); return i >= 0 ? sessions[i].userEmail : ""; }
// Allocate a slot — prefers empty, else evicts round-robin
static int allocSessionSlot() {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token.length() == 0) return i;
  }
  int slot = sessionNext;
  sessionNext = (sessionNext + 1) % MAX_SESSIONS;
  return slot;
}

static const char HTML_STYLE[] =
  "*{box-sizing:border-box;margin:0;padding:0;}"
  "body{font-family:'Segoe UI',Arial,sans-serif;background:#EEF4FB;min-height:100vh;font-size:17px;}"
  ".topbar{background:linear-gradient(135deg,#0D47A1,#1976D2);display:flex;align-items:center;"
    "justify-content:space-between;padding:0 32px;height:70px;box-shadow:0 2px 8px rgba(0,0,0,.2);}"
  ".brand{color:#fff;font-size:26px;font-weight:800;letter-spacing:2px;text-transform:uppercase;}"
  ".topbar nav a{color:rgba(255,255,255,.85);text-decoration:none;margin-left:18px;font-size:15px;"
    "padding:6px 12px;border-radius:4px;transition:background .2s;}"
  ".topbar nav a:hover{background:rgba(255,255,255,.18);color:#fff;}"
  ".container{max-width:760px;margin:28px auto;padding:0 16px;}"
  ".layout{display:flex;min-height:calc(100vh - 54px);}"
  ".sidebar{width:230px;background:#fff;border-right:1.5px solid #BBDEFB;flex-shrink:0;padding:20px 0;}"
  ".sidebar-section{padding:8px 18px 6px;font-size:13px;font-weight:700;color:#90A4AE;"
    "text-transform:uppercase;letter-spacing:1px;}"
  ".sidebar-item{display:flex;align-items:center;gap:11px;padding:12px 20px;cursor:pointer;"
    "color:#546E7A;font-size:16px;font-weight:500;border-left:3px solid transparent;"
    "transition:all .15s;user-select:none;}"
  ".sidebar-item:hover{background:#E3F2FD;color:#1565C0;border-left-color:#90CAF9;}"
  ".sidebar-item.active{background:#E3F2FD;color:#1565C0;border-left-color:#1565C0;font-weight:700;}"
  ".sidebar-icon{font-size:19px;width:22px;text-align:center;}"
  ".main-content{flex:1;padding:26px 30px;background:#EEF4FB;overflow-y:auto;}"
  ".tab-panel{display:none;}"
  ".tab-panel.active{display:block;}"
  ".section-title{font-size:21px;font-weight:700;color:#0D47A1;margin-bottom:4px;}"
  ".section-sub{font-size:15px;color:#78909C;margin-bottom:18px;}"
  ".divider{border:none;border-top:2px solid #E3F2FD;margin:24px 0;}"
  ".page-title{margin-bottom:20px;}"
  ".page-title h1{color:#0D47A1;font-size:24px;font-weight:700;}"
  ".subtitle{color:#546E7A;font-size:15px;margin-top:3px;}"
  ".card{background:#fff;border-radius:10px;box-shadow:0 2px 12px rgba(13,71,161,.09);"
    "padding:28px 30px;margin-bottom:20px;}"
  "table{width:100%;border-collapse:collapse;background:#fff;border-radius:10px;overflow:hidden;"
    "box-shadow:0 2px 12px rgba(13,71,161,.09);margin-bottom:16px;}"
  "thead tr{background:linear-gradient(90deg,#0D47A1,#1976D2);}"
  "thead th{color:#fff;padding:12px 16px;font-size:15px;font-weight:600;text-align:left;letter-spacing:.3px;}"
  "tbody tr{border-bottom:1px solid #E3F2FD;transition:background .15s;}"
  "tbody tr:last-child td{border-bottom:none;}"
  "tbody tr:hover{background:#F0F7FF;}"
  "td{padding:11px 16px;color:#37474F;font-size:15px;}"
  ".btn{display:inline-flex;align-items:center;gap:5px;padding:7px 16px;border:none;border-radius:6px;"
    "cursor:pointer;font-size:15px;font-weight:600;text-decoration:none;transition:all .2s;line-height:1.5;}"
  ".btn-primary{background:#1565C0;color:#fff;}"
  ".btn-primary:hover{background:#0D47A1;box-shadow:0 3px 10px rgba(13,71,161,.35);}"
  ".btn-outline{background:#fff;color:#1565C0;border:1.5px solid #1565C0;}"
  ".btn-outline:hover{background:#E3F2FD;}"
  ".btn-edit{background:#fff;color:#1565C0;border:1.5px solid #1565C0;}"
  ".btn-edit:hover{background:#E3F2FD;}"
  ".btn-danger{background:#E53935;color:#fff;}"
  ".btn-danger:hover{background:#C62828;box-shadow:0 3px 10px rgba(198,40,40,.3);}"
  ".btn-cancel{background:#fff;color:#607D8B;border:1.5px solid #CFD8DC;}"
  ".btn-cancel:hover{background:#F5F5F5;}"
  ".action-bar{display:flex;gap:10px;margin-bottom:20px;}"
  ".form-group{margin-bottom:16px;}"
  ".form-group label{display:block;font-size:15px;font-weight:600;color:#37474F;margin-bottom:5px;}"
  ".form-group input{width:100%;padding:9px 12px;border:1.5px solid #BBDEFB;border-radius:6px;"
    "font-size:16px;color:#263238;outline:none;transition:border .2s,box-shadow .2s;}"
  ".form-group input:focus{border-color:#1565C0;box-shadow:0 0 0 3px rgba(21,101,192,.12);}"
  ".form-actions{margin-top:22px;display:flex;gap:10px;}"
  ".badge{display:inline-block;background:#E3F2FD;color:#1565C0;"
    "font-size:14px;font-weight:600;padding:4px 12px;border-radius:20px;}"
  ".alert{padding:13px 16px;border-radius:8px;font-size:16px;margin-bottom:16px;}"
  ".alert-info{background:#E3F2FD;color:#0D47A1;border-left:4px solid #1565C0;}"
  ".alert-danger{background:#FFEBEE;color:#B71C1C;border-left:4px solid #E53935;}"
  ".del-box{text-align:center;padding:10px 0 20px;}"
  ".del-icon{font-size:42px;margin-bottom:12px;}"
  ".del-msg{color:#546E7A;font-size:17px;margin-bottom:4px;}"
  ".del-email{font-weight:700;color:#263238;font-size:17px;margin-bottom:22px;}"
  ".del-btns{display:flex;justify-content:center;gap:12px;}"
  ".table-wrap{overflow-x:auto;-webkit-overflow-scrolling:touch;border-radius:10px;margin-bottom:16px;}"
  /* ---- Responsive ---- */
  "@media(max-width:900px){"
    ".sidebar{width:190px;}"
    ".main-content{padding:20px 18px;}"
  "}"
  "@media(max-width:660px){"
    ".topbar{padding:0 14px;height:56px;}"
    ".brand{font-size:18px;letter-spacing:1px;}"
    ".layout{flex-direction:column;min-height:unset;}"
    ".sidebar{width:100%;border-right:none;border-bottom:1.5px solid #BBDEFB;"
      "padding:4px 0;display:flex;flex-wrap:wrap;}"
    ".sidebar-section{width:100%;padding:5px 14px 2px;}"
    ".sidebar-item{flex:1 1 120px;padding:9px 10px;border-left:none;"
      "border-bottom:3px solid transparent;justify-content:center;font-size:15px;}"
    ".sidebar-item.active{border-left:none;border-bottom-color:#1565C0;}"
    ".sidebar-icon{display:none;}"
    ".main-content{padding:14px 10px;}"
    ".card{padding:16px 12px;}"
    ".section-title{font-size:18px;}"
    "table{font-size:14px;}"
    "td,thead th{padding:8px 8px;}"
    ".btn{padding:6px 11px;font-size:14px;}"
    ".form-actions{flex-wrap:wrap;}"
    ".del-btns{flex-wrap:wrap;}"
  "}"
  /* ---- User dropdown menu ---- */
  ".user-menu{position:relative;}"
  ".user-btn{display:flex;align-items:center;gap:8px;background:rgba(255,255,255,.15);"
    "color:#fff;border:1.5px solid rgba(255,255,255,.45);border-radius:8px;"
    "padding:7px 14px;cursor:pointer;font-size:15px;font-weight:600;"
    "font-family:inherit;outline:none;transition:background .2s;}"
  ".user-btn:hover{background:rgba(255,255,255,.25);}"
  ".user-avatar{width:28px;height:28px;border-radius:50%;background:rgba(255,255,255,.28);"
    "display:flex;align-items:center;justify-content:center;font-size:14px;font-weight:800;color:#fff;}"
  ".user-dropdown{display:none;position:absolute;right:0;top:calc(100% + 8px);"
    "background:#fff;border-radius:10px;box-shadow:0 6px 28px rgba(13,71,161,.18);"
    "min-width:180px;z-index:999;overflow:hidden;}"
  ".user-dropdown.open{display:block;}"
  ".user-dropdown a{display:flex;align-items:center;gap:10px;padding:12px 18px;"
    "color:#37474F;text-decoration:none;font-size:15px;font-weight:500;"
    "border-bottom:1px solid #EEF4FB;transition:background .15s;}"
  ".user-dropdown a:last-child{border-bottom:none;}"
  ".user-dropdown a:hover{background:#EEF4FB;color:#1565C0;}"
  ".dd-danger{color:#E53935 !important;}"
  ".user-dropdown .dd-danger:hover{background:#FFF5F5;color:#C62828 !important;}"
  "@media(max-width:660px){"
    ".user-btn .uname{display:none;}"
    ".user-btn .arr{display:none;}"
  "}"
  /* ---- Home Overview Cards ---- */
  ".home-grid{display:grid;grid-template-columns:1fr 1fr;gap:20px;margin-bottom:24px;}"
  ".home-card{background:#fff;border-radius:14px;box-shadow:0 3px 18px rgba(13,71,161,.11);"
    "padding:26px 28px;border-top:4px solid #E0E0E0;}"
  ".home-card-title{font-size:11px;font-weight:700;color:#90A4AE;text-transform:uppercase;"
    "letter-spacing:1.4px;margin-bottom:14px;}"
  ".home-val{font-size:28px;font-weight:800;color:#263238;margin-bottom:4px;}"
  ".home-sub{font-size:14px;color:#78909C;}"
  ".ble-pill{display:inline-flex;align-items:center;gap:7px;padding:6px 14px;"
    "border-radius:20px;font-size:15px;font-weight:700;}"
  ".ble-pill-on{background:#E8F5E9;color:#2E7D32;}"
  ".ble-pill-off{background:#ECEFF1;color:#607D8B;}"
  ".ble-pill-wait{background:#FFF3E0;color:#E65100;}"
  ".ble-dot{width:9px;height:9px;border-radius:50%;flex-shrink:0;}"
  ".ble-dot-on{background:#43A047;box-shadow:0 0 5px #66BB6A;}"
  ".ble-dot-off{background:#B0BEC5;}"
  ".ble-dot-wait{background:#FB8C00;box-shadow:0 0 5px #FFA726;}"
  ".motion-tag{display:inline-block;padding:5px 13px;border-radius:16px;"
    "font-size:14px;font-weight:700;margin:2px 3px;}"
  ".tag-up{background:#E3F2FD;color:#1565C0;}"
  ".tag-down{background:#E0F7FA;color:#00838F;}"
  ".tag-estop{background:#FFEBEE;color:#C62828;}"
  ".tag-idle{background:#ECEFF1;color:#78909C;}"
  "@media(max-width:660px){.home-grid{grid-template-columns:1fr;}}"
  /* ---- Output Config select ---- */
  ".oc-select{width:100%;padding:8px 10px;border:1.5px solid #BBDEFB;border-radius:6px;"
    "font-size:15px;color:#263238;outline:none;background:#fff;"
    "transition:border .2s,box-shadow .2s;}"
  ".oc-select:focus{border-color:#1565C0;box-shadow:0 0 0 3px rgba(21,101,192,.12);}"
  ".oc-row td{vertical-align:middle;padding:13px 16px;}"
  ".oc-motion{font-size:15px;font-weight:700;color:#37474F;}";

// ============================================================
//  Login page
// ============================================================
static String loginPage(const String& errMsg) {
  String html;
  html  = "<!DOCTYPE html><html lang='en'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Sign In | INTELLI RRC</title>";
  html += "<link rel='stylesheet' href='/style.css'>";
  html += "<style>"
    "body{display:flex;justify-content:center;align-items:center;min-height:100vh;"
    "background:linear-gradient(135deg,#0D47A1 0%,#1976D2 60%,#42A5F5 100%);}"
    ".login-box{background:#fff;border-radius:16px;"
    "box-shadow:0 8px 40px rgba(0,0,0,.25);padding:44px 40px;width:100%;max-width:380px;}"
    ".login-brand{text-align:center;color:#0D47A1;font-size:22px;font-weight:800;"
    "letter-spacing:2px;text-transform:uppercase;margin-bottom:4px;}"
    ".login-sub{text-align:center;color:#90A4AE;font-size:13px;margin-bottom:28px;}"
    "@media(max-width:440px){"
      ".login-box{padding:30px 18px;border-radius:10px;}"
      ".login-brand{font-size:18px;}"
    "}"
    "</style>";
  html += "</head><body>";
  html += "<div class='login-box'>";
  html += "<div class='login-brand'>&#9889; INTELLI RRC</div>";
  html += "<div class='login-sub'>PLC Control Panel</div>";
  if (errMsg.length() > 0) {
    html += "<div class='alert alert-danger'>" + errMsg + "</div>";
  }
  html += "<form method='POST' action='/login'>";
  html += "<div class='form-group'><label>Email Address</label>";
  html += "<input type='email' name='username' placeholder='Enter your email address' required autofocus></div>";
  html += "<div class='form-group' style='margin-bottom:24px;'><label>Password</label>";
  html += "<input type='password' name='password' placeholder='Enter password' required></div>";
  html += "<button type='submit' class='btn btn-primary' "
    "style='width:100%;justify-content:center;padding:11px;font-size:15px;'>"
    "&#128274; Sign In</button>";
  html += "</form></div></body></html>";
  return html;
}

// ============================================================
//  Page helpers
// ============================================================
static String pageHead(const char* title, const char* subtitle = nullptr) {
  String h;
  h  = "<!DOCTYPE html><html lang='en'><head>";
  h += "<meta charset='UTF-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>"; h += title; h += " | Intelli RRC</title>";
  h += "<link rel='stylesheet' href='/style.css'>";
  h += "</head><body>";
  h += "<div class='topbar'>";
  h += "<span class='brand'>INTELLI RRC</span>";
  h += "<div class='user-menu'>";
  h += "<button class='user-btn' onclick='toggleMenu(event)'>";
  {
    String _dn;
    String _sue = currentUserEmail();
    if (_sue.length() > 0) {
      // Show name for BLE users
      _dn = _sue; // fallback to email
      for (int _i = 0; _i < userCount; _i++) {
        if (String(users[_i].email).equalsIgnoreCase(_sue)) {
          _dn = String(users[_i].name);
          break;
        }
      }
    } else {
      _dn = String(webAdminUser);
    }
    h += "<div class='user-avatar'>" + String((char)toupper(_dn[0])) + "</div>";
    h += "<span class='uname'>" + _dn + "</span>";
  }
  h += "<span class='arr' style='font-size:10px;opacity:.75;'>&#9660;</span>";
  h += "</button>";
  h += "<div class='user-dropdown' id='userDrop'>";
  h += "<a href='/account'>&#9881; Account Settings</a>";
  h += "<a href='/logout' class='dd-danger'>&#x23FB; Logout</a>";
  h += "</div></div>";
  h += "</div><div class='container'>";
  h += "<div class='page-title'><h1>"; h += title; h += "</h1>";
  if (subtitle) { h += "<p class='subtitle'>"; h += subtitle; h += "</p>"; }
  h += "</div>";
  return h;
}

static String pageFoot() {
  return "<script>"
    "function toggleMenu(e){e.stopPropagation();"
    "document.getElementById('userDrop').classList.toggle('open');}"
    "document.addEventListener('click',function(){"
    "var d=document.getElementById('userDrop');if(d)d.classList.remove('open');});"
    "</script>"
    "</div></body></html>";
}

static String errorPage(String msg, const char* backUrl) {
  String h = pageHead("Error");
  h += "<div class='card'><div class='alert alert-danger'>"; h += msg; h += "</div>";
  h += "<a href='"; h += backUrl; h += "' class='btn btn-cancel'>&#8592; Back</a>";
  h += "</div>"; h += pageFoot();
  return h;
}

// ============================================================
//  Auth — cookie session
// ============================================================
static bool webAuth() {
  if (findSessionIdx() >= 0) {
    return true;
  }
  webServer.sendHeader("Location", "/login");
  webServer.send(302);
  return false;
}

// Accepts Admin or Operator for Output Config
static bool webAuthOutputConfig() {
  if (!webAuth()) return false;
  String role = currentRole();
  if (role != "Admin" && role != "Operator") {
    webServer.send(403, "text/html", errorPage("Access denied. Operator or Admin privileges required.", "/"));
    return false;
  }
  return true;
}

static bool webAuthAdmin() {
  if (!webAuth()) return false;
  if (currentRole() != "Admin") {
    webServer.send(403, "text/html", errorPage("Access denied. Admin privileges required.", "/"));
    return false;
  }
  return true;
}

// ============================================================
//  Login / Logout
// ============================================================
static void handleLoginGet() {
  if (findSessionIdx() >= 0) {
    webServer.sendHeader("Location", "/");
    webServer.send(302);
    return;
  }
  webServer.send(200, "text/html", loginPage(""));
}

static void handleLoginPost() {
  String username = webServer.arg("username");
  String password = webServer.arg("password");
  // Web admin credentials
  if (String(webAdminUser).equalsIgnoreCase(username) && password == webAdminPass) {
    int _slot = allocSessionSlot();
    sessions[_slot].token     = generateToken();
    sessions[_slot].role      = "Admin";
    sessions[_slot].userEmail = "";
    webServer.sendHeader("Set-Cookie", "session=" + sessions[_slot].token + "; HttpOnly; Path=/");
    webServer.sendHeader("Location", "/");
    webServer.send(303);
    return;
  }
  // BLE user credentials — match by email only
  for (int i = 0; i < userCount; i++) {
    bool emailMatch = String(users[i].email).equalsIgnoreCase(username);
    if (emailMatch && String(users[i].password) == password) {
      int _slot = allocSessionSlot();
      sessions[_slot].token     = generateToken();
      sessions[_slot].role      = String(users[i].role);
      sessions[_slot].userEmail = String(users[i].email);
      webServer.sendHeader("Set-Cookie", "session=" + sessions[_slot].token + "; HttpOnly; Path=/");
      webServer.sendHeader("Location", "/");
      webServer.send(303);
      return;
    }
  }
  webServer.send(401, "text/html", loginPage("Invalid email address or password."));
}

static void handleLogout() {
  int _si = findSessionIdx();
  if (_si >= 0) { sessions[_si].token = ""; sessions[_si].role = ""; sessions[_si].userEmail = ""; }
  webServer.sendHeader("Set-Cookie", "session=; HttpOnly; Path=/; Max-Age=0");
  webServer.sendHeader("Location", "/login");
  webServer.send(303);
}

// ============================================================
//  Dashboard  —  two-column layout
// ============================================================
static void handleRoot() {
  if (!webAuth()) return;

  String role = currentRole();
  bool isAdmin = (role == "Admin");
  bool isOperator = (role == "Operator");
  String defaultTab = "home";
  String activeTab = webServer.hasArg("tab") ? webServer.arg("tab") : defaultTab;
  if (!isAdmin && activeTab == "users") activeTab = "home";
  if (activeTab != "home" && activeTab != "users" && activeTab != "outconfig" && activeTab != "network") activeTab = defaultTab;
  bool isHome      = (activeTab == "home");
  bool isUsers     = (activeTab == "users");
  bool isOutConfig = (activeTab == "outconfig");
  bool isNetwork   = (activeTab == "network");

  // Start chunked response immediately so the browser doesn't time out while
  // we build the large HTML string.
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");

  // Helper lambda: flush html to client whenever it exceeds 4 KB,
  // then clear the buffer.
  String html;
  html.reserve(4096);
  auto flush = [&]() {
    if (html.length() > 0) {
      webServer.sendContent(html);
      html = "";
    }
  };
  auto mayFlush = [&]() {
    if (html.length() >= 4096) flush();
  };
  html  = "<!DOCTYPE html><html lang='en'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Intelli RRC</title>";
  html += "<link rel='stylesheet' href='/style.css'>";
  html += "</head><body>";
  mayFlush();  // flush <head>+styles immediately

  // Topbar
  html += "<div class='topbar'>";
  html += "<span class='brand'>INTELLI RRC</span>";
  html += "<div class='user-menu'>";
  html += "<button class='user-btn' onclick='toggleMenu(event)'>";
  {
    String _dn;
    String _sue2 = currentUserEmail();
    if (_sue2.length() > 0) {
      _dn = _sue2; // fallback
      for (int _i = 0; _i < userCount; _i++) {
        if (String(users[_i].email).equalsIgnoreCase(_sue2)) {
          _dn = String(users[_i].name);
          break;
        }
      }
    } else {
      _dn = String(webAdminUser);
    }
    html += "<div class='user-avatar'>" + String((char)toupper(_dn[0])) + "</div>";
    html += "<span class='uname'>" + _dn + "</span>";
  }
  html += "<span class='arr' style='font-size:10px;opacity:.75;'>&#9660;</span>";
  html += "</button>";
  html += "<div class='user-dropdown' id='userDrop'>";
  html += "<a href='/account'>&#9881; Account Settings</a>";
  html += "<a href='/logout' class='dd-danger'>&#x23FB; Logout</a>";
  html += "</div></div>";
  html += "</div>";

  // Two-column layout
  html += "<div class='layout'>";

  // --- Sidebar ---
  html += "<div class='sidebar'>";
  html += "<div class='sidebar-section'>Navigation</div>";
  html += "<div class='sidebar-item" + String(isHome ? " active" : "") + "' id='nav-home' onclick=\"showTab('home')\"\u003e<span class='sidebar-icon'>&#127968;</span>Home</div>";
  if (isAdmin) {
    html += "<div class='sidebar-item" + String(isUsers ? " active" : "") + "' id='nav-users' onclick=\"showTab('users')\"\u003e<span class='sidebar-icon'>&#128100;</span>User Management</div>";
  }
  if (isAdmin || isOperator) {
    html += "<div class='sidebar-item" + String(isOutConfig ? " active" : "") + "' id='nav-outconfig' onclick=\"showTab('outconfig')\"\u003e<span class='sidebar-icon'>&#9881;</span>Output Config</div>";
    html += "<div class='sidebar-item" + String(isNetwork ? " active" : "") + "' id='nav-network' onclick=\"showTab('network')\"\u003e<span class='sidebar-icon'>&#128246;</span>Network Settings</div>";
  }
  html += "</div>";
  mayFlush();  // flush sidebar

  // --- Main content ---
  html += "<div class='main-content'>";

  // ====== HOME PANEL ======
  if (isHome) {
  html += "<div id='tab-home' class='tab-panel active'>";
  html += "<div class='section-title'>&#127968; Home</div>";
  html += "<div class='home-grid'>";

  // --- BLE Connection Card ---
  {
    const char *pillCls, *dotCls, *bleLabel;
    String userLabel = "";
    if (deviceConnected && authenticated) {
      pillCls  = "ble-pill ble-pill-on";
      dotCls   = "ble-dot ble-dot-on";
      bleLabel = "Connected";
      if (connectedUserEmail[0] != '\0') {
        userLabel = String(connectedUserEmail);
        for (int _bi = 0; _bi < userCount; _bi++) {
          if (String(users[_bi].email).equalsIgnoreCase(connectedUserEmail)) {
            userLabel = String(users[_bi].name);
            break;
          }
        }
      }
    } else if (deviceConnected) {
      pillCls  = "ble-pill ble-pill-wait";
      dotCls   = "ble-dot ble-dot-wait";
      bleLabel = "Authenticating...";
    } else {
      pillCls  = "ble-pill ble-pill-off";
      dotCls   = "ble-dot ble-dot-off";
      bleLabel = "Not Connected";
    }
    html += "<div class='home-card' style='border-top:4px solid #43A047;'>";
    html += "<div class='home-card-title'>&#128241; BLE Connection</div>";
    html += "<div id='hble-conn' class='" + String(pillCls) + "'>";
    html += "<span class='" + String(dotCls) + "'></span>" + String(bleLabel) + "</div>";
    if (userLabel.length() > 0) {
      html += "<div id='hble-user' style='margin-top:10px;font-size:15px;"
              "color:#37474F;font-weight:600;'>User: " + userLabel + "</div>";
    } else {
      html += "<div id='hble-user' style='display:none;margin-top:10px;font-size:15px;"
              "color:#37474F;font-weight:600;'></div>";
    }
    html += "</div>";
  }

  // --- Motion Status Card ---
  {
    String tags = "";
    // Web status reflects E-STOP output state: HIGH means shown as active.
    if (!motionEstop) tags += "<span class='motion-tag tag-estop'>&#9888; E-STOP</span>";
    if (motionUp)      tags += "<span class='motion-tag tag-up'>&#9650; UP</span>";
    if (motionDown)    tags += "<span class='motion-tag tag-down'>&#9660; DOWN</span>";
    if (motionLeft)    tags += "<span class='motion-tag tag-up'>&#9664; LEFT</span>";
    if (motionRight)   tags += "<span class='motion-tag tag-down'>&#9654; RIGHT</span>";
    if (!motionUp && !motionDown && !motionLeft && !motionRight)
      tags += "<span class='motion-tag tag-idle'>No Active Motion</span>";
    html += "<div class='home-card' style='border-top:4px solid #E65100;'>";
    html += "<div class='home-card-title'>&#127959; Motion Status</div>";
    html += "<div id='hmot-status'>" + tags + "</div>";
    html += "</div>";
  }
  html += "</div>"; // end home-grid
  html += "</div>"; // end tab-home
  mayFlush();  // flush home panel
  }

  // ====== USERS PANEL ======
  if (isAdmin && isUsers) {
  html += "<div id='tab-users' class='tab-panel active'>";
  html += "<div class='section-title'>&#128100; User Management</div>";
  html += "<div class='section-sub'>List of registered users</div>";

  html += "<div class='table-wrap' style='position:relative;'>";
  // + button pinned to top-right of the table header row
  if (userCount < MAX_USERS) {
    html += "<a href='/add' title='Add New User' "
            "style='position:absolute;top:6px;right:8px;z-index:10;"
            "width:30px;height:30px;border-radius:50%;background:rgba(255,255,255,.22);"
            "color:#fff;display:flex;align-items:center;justify-content:center;"
            "font-size:20px;font-weight:700;text-decoration:none;line-height:1;"
            "border:1.5px solid rgba(255,255,255,.45);transition:background .2s;'"
            " onmouseover=\"this.style.background='rgba(255,255,255,.38)'\""
            " onmouseout=\"this.style.background='rgba(255,255,255,.22)'\""
            ">&#43;</a>";
  }
  html += "<table><thead><tr>"
          "<th width='36'>#</th><th>Name</th><th>Email</th><th>Role</th><th>Actions</th>"
          "</tr></thead><tbody>";
  for (int i = 0; i < userCount; i++) {
    html += "<tr><td>" + String(i + 1) + "</td>";
    html += "<td>" + String(users[i].name)  + "</td>";
    html += "<td>" + String(users[i].email) + "</td>";
    html += "<td><span class='badge'>" + String(users[i].role) + "</span></td>";
    html += "<td>";
    html += "<a href='/edit?idx=" + String(i) + "' class='btn btn-edit'>&#9998; Edit</a>&nbsp;";
    html += "<a href='/confirm-delete?idx=" + String(i) + "' class='btn btn-danger'>Delete</a>&nbsp;";
    html += "<a href='/login-as?idx=" + String(i) + "' class='btn btn-outline' title='Login as this user'>&#128100; Login As</a>";
    html += "</td></tr>";
  }
  if (userCount == 0) {
    html += "<tr><td colspan='5' style='text-align:center;color:#90A4AE;padding:20px;'>"
            "No users yet. Press &#43; to add one.</td></tr>";
  }
  html += "</tbody></table>";
  html += "</div>"; // end table-wrap
  html += "<div style='margin-bottom:20px;'><span class='badge'>&#128100; "
          + String(userCount) + " / " + String(MAX_USERS) + " users</span></div>";

  html += "</div>"; // end tab-users
  } // end if(isAdmin) users panel
  mayFlush();  // flush users panel

  // ====== OUTPUT CONFIG PANEL ======
  if ((isAdmin || isOperator) && isOutConfig) {
    bool savedOk = webServer.hasArg("saved");
    bool dupErr  = webServer.hasArg("err") && webServer.arg("err") == "dup";
    html += "<div id='tab-outconfig' class='tab-panel active'>";
    html += "<div class='section-title'>&#9881; Output Configuration</div>";
    html += "<div class='section-sub'>Assign PLC output pins to each motion function.</div>";
    if (savedOk) {
      html += "<div class='alert alert-info'>&#10003; Configuration saved successfully.</div>";
    }
    if (dupErr) {
      html += "<div class='alert alert-danger'>&#9888; Save failed: two motion functions cannot use the same output pin.</div>";
    }
    html += "<form id='oc-form' method='POST' action='/outconfig'>";
    html += "<div class='card' style='padding:0;overflow:hidden;'>";
    html += "<table>";
    html += "<thead><tr><th>Motion Function</th><th>Current Pin</th><th>Assign To</th></tr></thead>";
    html += "<tbody>";

    // Helper lambda to build one select dropdown row
    struct RowDef { const char* label; uint8_t idx; };
    RowDef rows[5] = {
      { "&#9888; E-STOP", outIdxEstop },
      { "&#9650; UP",     outIdxUp    },
      { "&#9660; DOWN",   outIdxDown  },
      { "&#9664; LEFT",   outIdxLeft  },
      { "&#9654; RIGHT",  outIdxRight }
    };
    const char* names[5] = { "estop", "up", "down", "left", "right" };
    for (int r = 0; r < 5; r++) {
      html += "<tr class='oc-row'>";
      html += "<td class='oc-motion'>" + String(rows[r].label) + "</td>";
      html += "<td><span class='badge oc-current'>" + String(rows[r].idx < NUM_OUTPUT_PINS ? OUTPUT_PINS[rows[r].idx].label : "None") + "</span></td>";
      html += "<td><select name='" + String(names[r]) + "' class='oc-select'>";
      // None option first
      html += "<option value='" + String(NUM_OUTPUT_PINS) + "'"
              + String(rows[r].idx >= NUM_OUTPUT_PINS ? " selected" : "") + ">" 
              + "&#8212; None &#8212;</option>";
      for (int p = 0; p < NUM_OUTPUT_PINS; p++) {
        html += "<option value='" + String(p) + "'"
                + String(p == rows[r].idx ? " selected" : "") + ">"
                + String(OUTPUT_PINS[p].label) + "</option>";
      }
      html += "</select></td></tr>";
    }

    html += "</tbody></table>";
    html += "</div>"; // end card
    html += "<div id='oc-save-msg' class='alert alert-info' style='display:none;margin-top:14px;'></div>";
    html += "<div class='form-actions' style='margin-top:18px;'>";
    html += "<button id='oc-save-btn' type='submit' class='btn btn-primary'>&#10003; Save Configuration</button>";
    html += "</div>";
    html += "</form>";
    html += "</div>"; // end tab-outconfig
    mayFlush();  // flush outconfig panel
  }

  // ====== NETWORK SETTINGS PANEL ======
  if ((isAdmin || isOperator) && isNetwork) {
    bool savedOk = webServer.hasArg("saved");
    html += "<div id='tab-network' class='tab-panel active'>";
    html += "<div class='section-title'>&#128246; Network Settings</div>";
    html += "<div class='section-sub'>Configure BLE name, WiFi SSID and password.</div>";
    if (savedOk) {
      html += "<div class='alert alert-info'>&#10003; Network settings saved. Restarting device...</div>";
    }
    html += "<form id='net-form' method='POST' action='/netconfig'>";
    html += "<div class='card' style='padding:0;overflow:hidden;'>";
    html += "<div style='padding:18px;'>";
    html += "<div class='form-group'><label>BLE Device Name</label>";
    html += "<input type='text' name='ble_name' value='" + String(bleName) + "' placeholder='RRC_' minlength='5' maxlength='10' required pattern='RRC_.{1,6}'>";
    html += "</div>";
    html += "<div class='form-group'><label>WiFi SSID</label>";
    html += "<input type='text' name='wifi_ssid' value='" + String(wifiSsid) + "' minlength='5' maxlength='10' required>";
    html += "</div>";
    html += "<div class='form-group'><label>WiFi Password</label>";
    html += "<input id='wifi-password' type='password' name='wifi_password' value='" + String(wifiPassword) + "' minlength='8' maxlength='10' required>";
    html += "<div style=\"margin-top:8px;display:flex;align-items:center;gap:8px;font-size:14px;color:#37474F;\">";
    html += "<input id='toggle-wifi-password' type='checkbox' onchange='toggleWifiPassword()'>";
    html += "<label for='toggle-wifi-password' style='margin:0;cursor:pointer;'>Show password</label>";
    html += "</div>";
    html += "</div>";
    html += "<div style='font-size:13px;color:#546E7A;margin-bottom:12px;'>The device will restart automatically after saving new BLE or WiFi credentials.</div>";
    html += "</div>";
    html += "</div>"; // end card
    html += "<div class='form-actions' style='margin-top:18px;'>";
    html += "<button id='net-save-btn' type='submit' class='btn btn-primary'>&#10003; Save Network Settings</button>";
    html += "</div>";
    html += "</form>";
    html += "</div>"; // end tab-network
    mayFlush();
  }

  html += "</div>"; // end main-content
  html += "</div>"; // end layout

  // JavaScript: tab switching
  html += "<script>";
  html += "function showTab(t){";
  html += "window.location='/?tab='+encodeURIComponent(t);}";
  html += "function toggleMenu(e){e.stopPropagation();"
          "document.getElementById('userDrop').classList.toggle('open');}"
          "document.addEventListener('click',function(){"
          "var d=document.getElementById('userDrop');if(d)d.classList.remove('open');});";

  if (isHome) {
    // Home tab live updater
    html += "function updateHomeUI(s){";
    html += "var bc=document.getElementById('hble-conn');";
    html += "var bu=document.getElementById('hble-user');";
    html += "if(bc){";
    html += "if(s.ble_conn===1&&s.ble_auth===1){";
    html += "bc.className='ble-pill ble-pill-on';";
    html += "bc.innerHTML=\"<span class='ble-dot ble-dot-on'></span>Connected\";";
    html += "if(bu){bu.style.display='block';bu.textContent='User: '+(s.ble_user||'Unknown');}";
    html += "}else if(s.ble_conn===1){";
    html += "bc.className='ble-pill ble-pill-wait';";
    html += "bc.innerHTML=\"<span class='ble-dot ble-dot-wait'></span>Authenticating...\";";
    html += "if(bu)bu.style.display='none';";
    html += "}else{";
    html += "bc.className='ble-pill ble-pill-off';";
    html += "bc.innerHTML=\"<span class='ble-dot ble-dot-off'></span>Not Connected\";";
    html += "if(bu)bu.style.display='none';}}";
    html += "var ms=document.getElementById('hmot-status');";
    html += "if(ms){";
    html += "var mt='';";
    html += "if(s.estop===1){mt+=\"<span class='motion-tag tag-estop'>&#9888; E-STOP</span>\";}";
    html += "if(s.up===1)mt+=\"<span class='motion-tag tag-up'>&#9650; UP</span>\";";
    html += "if(s.down===1)mt+=\"<span class='motion-tag tag-down'>&#9660; DOWN</span>\";";
    html += "if(s.left===1)mt+=\"<span class='motion-tag tag-up'>&#9664; LEFT</span>\";";
    html += "if(s.right===1)mt+=\"<span class='motion-tag tag-down'>&#9654; RIGHT</span>\";";
    html += "if(!s.up&&!s.down&&!s.left&&!s.right)mt+=\"<span class='motion-tag tag-idle'>No Active Motion</span>\";";
    html += "ms.innerHTML=mt;}}";
    html += "setInterval(function(){";
    html += "fetch('/motion/status',{cache:'no-store'}).then(function(r){return r.json();})";
    html += ".then(function(s){updateHomeUI(s);}).catch(function(){});";
    html += "},1000);";
  }

  if (isOutConfig) {
    // Output config save without full page reload
    html += "(function(){";
    html += "var f=document.getElementById('oc-form');if(!f)return;";
    html += "var b=document.getElementById('oc-save-btn');";
    html += "var m=document.getElementById('oc-save-msg');";
    html += "function setMsg(ok,msg){if(!m)return;m.style.display='block';m.className='alert '+(ok?'alert-info':'alert-danger');m.textContent=msg;}";
    html += "f.addEventListener('submit',function(e){e.preventDefault();";
    html += "var estop=f.elements['estop'].value;var up=f.elements['up'].value;var down=f.elements['down'].value;";
    html += "var left=f.elements['left'].value;var right=f.elements['right'].value;";
    html += "var noneVal='" + String(NUM_OUTPUT_PINS) + "';";
    html += "if((estop!==noneVal&&estop===up)||(estop!==noneVal&&estop===down)||(estop!==noneVal&&estop===left)||(estop!==noneVal&&estop===right)||(up!==noneVal&&up===down)||(up!==noneVal&&up===left)||(up!==noneVal&&up===right)||(down!==noneVal&&down===left)||(down!==noneVal&&down===right)||(left!==noneVal&&left===right)){setMsg(false,'Save failed: two motion functions cannot use the same output pin.');return;}";
    html += "if(b)b.disabled=true;";
    html += "var body=new URLSearchParams(new FormData(f)).toString();";
    html += "fetch('/outconfig',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Requested-With':'fetch'},body:body})";
    html += ".then(function(r){return r.json().then(function(j){return {ok:r.ok,body:j};});})";
    html += ".then(function(x){";
    html += "if(!x.ok||!x.body||!x.body.ok)throw new Error((x.body&&x.body.msg)?x.body.msg:'Save failed. Please try again.');";
    html += "setMsg(true,'Configuration saved successfully.');";
    html += "var rows=f.querySelectorAll('tr.oc-row');";
    html += "rows.forEach(function(row){var sel=row.querySelector('select');var cur=row.querySelector('.oc-current');if(sel&&cur){cur.textContent=sel.options[sel.selectedIndex].text.replace(/—/g,'').trim()||'None';}});";
    html += "})";
    html += ".catch(function(err){setMsg(false,(err&&err.message)?err.message:'Save failed. Please try again.');})";
    html += ".finally(function(){if(b)b.disabled=false;});";
    html += "});";
    html += "})();";
  }
  if (isNetwork) {
    html += "function toggleWifiPassword(){";
    html += "var pw=document.getElementById('wifi-password');";
    html += "if(!pw)return;";
    html += "pw.type = (pw.type === 'password' ? 'text' : 'password');";
    html += "}";
  }
  html += "</script>";

  html += "</body></html>";
  flush();  // send final chunk and close chunked response
  webServer.sendContent("");  // zero-length chunk = end of chunked transfer
}

// ============================================================
//  Add User
// ============================================================
static void handleAddGet() {
  if (!webAuthAdmin()) return;
  if (userCount >= MAX_USERS) {
    webServer.send(200, "text/html",
      pageHead("Add User", "Max users reached") +
      "<div class='card'><div class='alert alert-info'>Maximum users reached (" +
      String(MAX_USERS) + ").</div>"
      "<a href='/?tab=users' class='btn btn-cancel'>&#8592; Back</a></div>" +
      pageFoot());
    return;
  }
  String html = pageHead("Add User", "Create a new BLE login credential");
  html += "<div class='card'>";
  html += "<form method='POST' action='/add'>";
  html += "<div class='form-group'><label>Full Name</label>";
  html += "<input type='text' name='name' placeholder='e.g. John Smith' required autofocus></div>";
  html += "<div class='form-group'><label>Email Address</label>";
  html += "<input type='email' name='email' placeholder='user@example.com' required></div>";
  html += "<div class='form-group'><label>Password</label>";
  html += "<input type='password' name='password' placeholder='Minimum 6 characters' "
          "required minlength='6'></div>";
  html += "<div class='form-group'><label>Role</label>";
  html += "<select name='role' style='width:100%;padding:9px 12px;border:1.5px solid #BBDEFB;"
          "border-radius:6px;font-size:14px;color:#263238;outline:none;background:#fff;'>";
  html += "<option value='Operator'>Operator</option>";
  html += "<option value='Admin'>Admin</option>";
  html += "</select></div>";
  html += "<div class='form-actions'>";
  html += "<button type='submit' class='btn btn-primary'>&#10003; Add User</button>";
  html += "<a href='/?tab=users' class='btn btn-cancel'>&#8592; Cancel</a>";
  html += "</div></form></div>";
  html += pageFoot();
  webServer.send(200, "text/html", html);
}

static void handleAddPost() {
  if (!webAuthAdmin()) return;
  if (userCount >= MAX_USERS) {
    webServer.send(400, "text/html", errorPage("Max users reached.", "/"));
    return;
  }
  String name     = webServer.arg("name");
  String email    = webServer.arg("email");
  String password = webServer.arg("password");
  String role     = webServer.arg("role");
  if (name.length() == 0 || email.length() == 0 || password.length() == 0) {
    webServer.send(400, "text/html", errorPage("Name, email and password are required.", "/add"));
    return;
  }
  if (role != "Admin" && role != "Operator") role = "Operator";
  for (int i = 0; i < userCount; i++) {
    if (email.equalsIgnoreCase(users[i].email)) {
      webServer.send(400, "text/html", errorPage("Email already exists.", "/add"));
      return;
    }
  }
  name.toCharArray(users[userCount].name,         sizeof(users[userCount].name));
  email.toCharArray(users[userCount].email,        sizeof(users[userCount].email));
  password.toCharArray(users[userCount].password,  sizeof(users[userCount].password));
  role.toCharArray(users[userCount].role,          sizeof(users[userCount].role));
  userCount++;
  saveUsers();
  Serial.printf("Web: user added — %s\n", users[userCount - 1].email);
  webServer.sendHeader("Location", "/?tab=users");
  webServer.send(303);
}

// ============================================================
//  Edit User
// ============================================================
static void handleEditGet() {
  if (!webAuthAdmin()) return;
  int idx = webServer.arg("idx").toInt();
  if (idx < 0 || idx >= userCount) {
    webServer.send(404, "text/html", errorPage("User not found.", "/"));
    return;
  }
  String html = pageHead("Edit User", "Update login credentials");
  html += "<div class='card'><form method='POST' action='/edit'>";
  html += "<input type='hidden' name='idx' value='" + String(idx) + "'>";
  html += "<div class='form-group'><label>Full Name</label>"
          "<input type='text' name='name' value='" + String(users[idx].name) + "' required></div>";
  html += "<div class='form-group'><label>Email Address</label>"
          "<input type='email' name='email' value='" + String(users[idx].email) + "' required></div>";
  html += "<div class='form-group'><label>Password</label>"
          "<input type='text' name='password' value='" + String(users[idx].password) + "' minlength='6'></div>";
  html += "<div class='form-group'><label>Role</label>";
  html += "<select name='role' style='width:100%;padding:9px 12px;border:1.5px solid #BBDEFB;"
          "border-radius:6px;font-size:14px;color:#263238;outline:none;background:#fff;'>";
  const char* roles[] = {"Operator", "Admin"};
  for (int r = 0; r < 2; r++) {
    html += "<option value='" + String(roles[r]) + "'";
    if (String(users[idx].role) == roles[r]) html += " selected";
    html += ">" + String(roles[r]) + "</option>";
  }
  html += "</select></div>";
  html += "<div class='form-actions'>";
  html += "<button type='submit' class='btn btn-primary'>&#10003; Save Changes</button>";
  html += "<a href='/?tab=users' class='btn btn-cancel'>Cancel</a>";
  html += "</div></form></div>";
  html += pageFoot();
  webServer.send(200, "text/html", html);
}

static void handleEditPost() {
  if (!webAuthAdmin()) return;
  int idx = webServer.arg("idx").toInt();
  if (idx < 0 || idx >= userCount) {
    webServer.send(404, "text/html", errorPage("User not found.", "/"));
    return;
  }
  String name     = webServer.arg("name");
  String email    = webServer.arg("email");
  String password = webServer.arg("password");
  String role     = webServer.arg("role");
  if (name.length() == 0 || email.length() == 0) {
    webServer.send(400, "text/html", errorPage("Name and email are required.", "/"));
    return;
  }
  if (role != "Admin" && role != "Operator") role = "Operator";
  for (int i = 0; i < userCount; i++) {
    if (i != idx && email.equalsIgnoreCase(users[i].email)) {
      webServer.send(400, "text/html", errorPage("Email already in use by another user.", "/"));
      return;
    }
  }
  name.toCharArray(users[idx].name,   sizeof(users[idx].name));
  email.toCharArray(users[idx].email, sizeof(users[idx].email));
  role.toCharArray(users[idx].role,   sizeof(users[idx].role));
  if (password.length() > 0) {
    password.toCharArray(users[idx].password, sizeof(users[idx].password));
  }
  saveUsers();
  Serial.printf("Web: user edited — %s\n", users[idx].email);
  webServer.sendHeader("Location", "/?tab=users");
  webServer.send(303);
}

// ============================================================
//  Delete User
// ============================================================
static void handleConfirmDelete() {
  if (!webAuthAdmin()) return;
  int idx = webServer.arg("idx").toInt();
  if (idx < 0 || idx >= userCount) {
    webServer.send(404, "text/html", errorPage("User not found.", "/"));
    return;
  }
  String html = pageHead("Delete User", "This action cannot be undone");
  html += "<div class='card'><div class='del-box'>";
  html += "<div class='del-icon'>&#128465;</div>";
  html += "<p class='del-msg'>Are you sure you want to delete</p>";
  html += "<p class='del-email'>" + String(users[idx].email) + "</p>";
  html += "<form method='POST' action='/delete'>";
  html += "<input type='hidden' name='idx' value='" + String(idx) + "'>";
  html += "<div class='del-btns'>";
  html += "<button type='submit' class='btn btn-danger'>Yes, Delete</button>";
  html += "<a href='/?tab=users' class='btn btn-cancel'>Cancel</a>";
  html += "</div></form></div></div>";
  html += pageFoot();
  webServer.send(200, "text/html", html);
}

static void handleDeletePost() {
  if (!webAuthAdmin()) return;
  int idx = webServer.arg("idx").toInt();
  if (idx < 0 || idx >= userCount) {
    webServer.send(404, "text/html", errorPage("User not found.", "/"));
    return;
  }
  Serial.printf("Web: user deleted — %s\n", users[idx].email);
  for (int i = idx; i < userCount - 1; i++) {
    strncpy(users[i].name,     users[i + 1].name,     sizeof(users[i].name));
    strncpy(users[i].email,    users[i + 1].email,    sizeof(users[i].email));
    strncpy(users[i].password, users[i + 1].password, sizeof(users[i].password));
    strncpy(users[i].role,     users[i + 1].role,     sizeof(users[i].role));
  }
  userCount--;
  saveUsers();
  webServer.sendHeader("Location", "/?tab=users");
  webServer.send(303);
}

// ============================================================
//  Account GET — dedicated settings page
// ============================================================
static void handleAccountGet() {
  if (!webAuth()) return;
  String _se = currentUserEmail();
  bool isBleUser = _se.length() > 0;
  // For BLE users, show their name/email; for web admin show username
  String currentName = isBleUser ? "" : "";
  String currentId   = isBleUser ? _se : String(webAdminUser);
  if (isBleUser) {
    for (int i = 0; i < userCount; i++) {
      if (String(users[i].email).equalsIgnoreCase(_se)) {
        currentName = String(users[i].name);
        break;
      }
    }
  }
  String subtitle = isBleUser ? "Change your name and password" : "Change your web panel login credentials";
  String html = pageHead("Account Settings", subtitle.c_str());
  html += "<div class='card'>";
  html += "<form method='POST' action='/account'>";
  if (isBleUser) {
    html += "<div class='form-group'><label>Full Name</label>";
    html += "<input type='text' name='new_user' value='" + currentName + "' required></div>";
    html += "<div class='form-group'><label>Email</label>";
    html += "<input type='text' value='" + currentId + "' disabled style='width:100%;padding:9px 12px;"
            "border:1.5px solid #BBDEFB;border-radius:6px;font-size:14px;color:#90A4AE;background:#F5F5F5;'></div>";
  } else {
    html += "<div class='form-group'><label>Email Address</label>";
    html += "<input type='email' name='new_user' value='" + currentId + "' required></div>";
  }
  html += "<div class='form-group'><label>Current Password</label>";
  if (isBleUser) {
    for (int _i = 0; _i < userCount; _i++) {
      if (String(users[_i].email).equalsIgnoreCase(_se)) {
        html += "<input type='text' name='cur_pass' value='" + String(users[_i].password) + "' required></div>";
        break;
      }
    }
  } else {
    html += "<input type='text' name='cur_pass' value='" + String(webAdminPass) + "' required></div>";
  }
  html += "<div class='form-group'><label>New Password</label>";
  html += "<input type='password' name='new_pass' placeholder='Leave blank to keep current'></div>";
  html += "<div class='form-group'><label>Confirm New Password</label>";
  html += "<input type='password' name='cfm_pass' placeholder='Repeat new password'></div>";
  html += "<div class='form-actions'>";
  html += "<button type='submit' class='btn btn-primary'>&#10003; Save Changes</button>";
  html += "<a href='/' class='btn btn-cancel'>&#8592; Back</a>";
  html += "</div></form></div>";
  html += pageFoot();
  webServer.send(200, "text/html", html);
}

// ============================================================
//  Account POST — change credentials (web admin or BLE user)
// ============================================================
static void handleAccountPost() {
  if (!webAuth()) return;
  String newUser = webServer.arg("new_user");
  String curPass = webServer.arg("cur_pass");
  String newPass = webServer.arg("new_pass");
  String cfmPass = webServer.arg("cfm_pass");
  if (newUser.length() == 0) {
    webServer.send(400, "text/html", errorPage("Name/username cannot be empty.", "/account"));
    return;
  }
  String _se2 = currentUserEmail();
  bool isBleUser = _se2.length() > 0;
  if (isBleUser) {
    // Find the BLE user by session email
    int idx = -1;
    for (int i = 0; i < userCount; i++) {
      if (String(users[i].email).equalsIgnoreCase(_se2)) { idx = i; break; }
    }
    if (idx < 0) {
      webServer.send(404, "text/html", errorPage("User session invalid.", "/account"));
      return;
    }
    if (String(users[idx].password) != curPass) {
      webServer.send(401, "text/html", errorPage("Current password is incorrect.", "/account"));
      return;
    }
    if (newPass.length() > 0) {
      if (newPass != cfmPass) {
        webServer.send(400, "text/html", errorPage("New passwords do not match.", "/account"));
        return;
      }
      if (newPass.length() < 6) {
        webServer.send(400, "text/html", errorPage("New password must be at least 6 characters.", "/account"));
        return;
      }
      newPass.toCharArray(users[idx].password, sizeof(users[idx].password));
    }
    newUser.toCharArray(users[idx].name, sizeof(users[idx].name));
    saveUsers();
    Serial.printf("Web: user updated own account — %s\n", users[idx].email);
    webServer.sendHeader("Location", "/");
    webServer.send(303);
  } else {
    // Web admin credentials
    if (String(webAdminPass) != curPass) {
      webServer.send(401, "text/html", errorPage("Current password is incorrect.", "/account"));
      return;
    }
    if (newPass.length() > 0) {
      if (newPass != cfmPass) {
        webServer.send(400, "text/html", errorPage("New passwords do not match.", "/account"));
        return;
      }
      if (newPass.length() < 6) {
        webServer.send(400, "text/html", errorPage("New password must be at least 6 characters.", "/account"));
        return;
      }
      newPass.toCharArray(webAdminPass, sizeof(webAdminPass));
    }
    newUser.toCharArray(webAdminUser, sizeof(webAdminUser));
    saveWebAdmin();
    // Invalidate current session so admin re-logs in with new credentials
    int _si2 = findSessionIdx();
    if (_si2 >= 0) { sessions[_si2].token = ""; sessions[_si2].role = ""; sessions[_si2].userEmail = ""; }
    webServer.sendHeader("Set-Cookie", "session=; HttpOnly; Path=/; Max-Age=0");
    webServer.sendHeader("Location", "/login");
    webServer.send(303);
  }
}

// ============================================================
//  Login As (Admin only) — switch web session to a BLE user
// ============================================================
static void handleLoginAs() {
  if (!webAuthAdmin()) return;
  int idx = webServer.arg("idx").toInt();
  if (idx < 0 || idx >= userCount) {
    webServer.send(404, "text/html", errorPage("User not found.", "/"));
    return;
  }
  int _slot2 = findSessionIdx();
  if (_slot2 < 0) _slot2 = allocSessionSlot();
  sessions[_slot2].token     = generateToken();
  sessions[_slot2].role      = String(users[idx].role);
  sessions[_slot2].userEmail = String(users[idx].email);
  webServer.sendHeader("Set-Cookie", "session=" + sessions[_slot2].token + "; HttpOnly; Path=/");
  Serial.printf("Web: Admin switched session to user — %s (%s)\n", users[idx].email, users[idx].role);
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

// ============================================================
//  Network Settings POST — change BLE/WiFi credentials
// ============================================================
static void handleNetConfigPost() {
  if (!webAuthOutputConfig()) return;
  String newBleName = webServer.arg("ble_name");
  String newSsid    = webServer.arg("wifi_ssid");
  String newPass    = webServer.arg("wifi_password");

  newBleName.trim();
  newSsid.trim();
  newPass.trim();

  if (newBleName.length() < 5 || newBleName.length() > 10 || !newBleName.startsWith("RRC_")) {
    webServer.send(400, "text/html", errorPage("BLE name must start with RRC_ and be 5-10 characters long.", "/?tab=network"));
    return;
  }
  if (newSsid.length() < 5 || newSsid.length() > 10) {
    webServer.send(400, "text/html", errorPage("WiFi SSID must be 5-10 characters long.", "/?tab=network"));
    return;
  }
  if (newPass.length() < 8 || newPass.length() > 10) {
    webServer.send(400, "text/html", errorPage("WiFi Password must be 8-10 characters long.", "/?tab=network"));
    return;
  }

  newBleName.toCharArray(bleName, sizeof(bleName));
  newSsid.toCharArray(wifiSsid, sizeof(wifiSsid));
  newPass.toCharArray(wifiPassword, sizeof(wifiPassword));
  saveNetworkConfig();

  String html = pageHead("Network Settings", "BLE and WiFi credentials updated.");
  html += "<div class='card'>";
  html += "<div class='alert alert-info'>&#10003; Saved successfully. Restarting device with new settings...</div>";
  html += "<p>Please wait while the device restarts. Reconnect to the new WiFi SSID and BLE name after reboot.</p>";
  html += "</div>";
  html += pageFoot();
  webServer.send(200, "text/html", html);
  delay(500);
  ESP.restart();
}

// ============================================================
//  Motion Status  —  read-only JSON endpoint
// ============================================================
// ============================================================
//  Output Configuration  (POST /outconfig)
// ============================================================
static void handleOutConfigPost() {
  if (!webAuthOutputConfig()) return;
  String reqType = webServer.header("X-Requested-With");
  auto clamp = [](int v) -> uint8_t {
    // 0-4 = valid pin index, NUM_OUTPUT_PINS (5) = None/unassigned
    return (v >= 0 && v <= NUM_OUTPUT_PINS) ? (uint8_t)v : (uint8_t)NUM_OUTPUT_PINS;
  };
  uint8_t newEstop = clamp(webServer.arg("estop").toInt());
  uint8_t newUp    = clamp(webServer.arg("up").toInt());
  uint8_t newDown  = clamp(webServer.arg("down").toInt());
  uint8_t newLeft  = clamp(webServer.arg("left").toInt());
  uint8_t newRight = clamp(webServer.arg("right").toInt());

  bool dupEstopUp    = (newEstop < NUM_OUTPUT_PINS) && (newEstop == newUp);
  bool dupEstopDown  = (newEstop < NUM_OUTPUT_PINS) && (newEstop == newDown);
  bool dupEstopLeft  = (newEstop < NUM_OUTPUT_PINS) && (newEstop == newLeft);
  bool dupEstopRight = (newEstop < NUM_OUTPUT_PINS) && (newEstop == newRight);
  bool dupUpDown     = (newUp    < NUM_OUTPUT_PINS) && (newUp    == newDown);
  bool dupUpLeft     = (newUp    < NUM_OUTPUT_PINS) && (newUp    == newLeft);
  bool dupUpRight    = (newUp    < NUM_OUTPUT_PINS) && (newUp    == newRight);
  bool dupDownLeft   = (newDown  < NUM_OUTPUT_PINS) && (newDown  == newLeft);
  bool dupDownRight  = (newDown  < NUM_OUTPUT_PINS) && (newDown  == newRight);
  bool dupLeftRight  = (newLeft  < NUM_OUTPUT_PINS) && (newLeft  == newRight);
  if (dupEstopUp || dupEstopDown || dupEstopLeft || dupEstopRight || dupUpDown || dupUpLeft || dupUpRight || dupDownLeft || dupDownRight || dupLeftRight) {
    Serial.println("Out config rejected: duplicate output pin assignment");
    if (reqType == "fetch") {
      webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
      webServer.send(400, "application/json", "{\"ok\":0,\"msg\":\"Save failed: two motion functions cannot use the same output pin.\"}");
      return;
    }
    webServer.sendHeader("Location", "/?tab=outconfig&err=dup");
    webServer.send(303);
    return;
  }

  outIdxEstop = newEstop;
  outIdxUp    = newUp;
  outIdxDown  = newDown;
  outIdxLeft  = newLeft;
  outIdxRight = newRight;
  saveOutputConfig();
  auto pl = [](uint8_t i) -> const char* {
    return (i < NUM_OUTPUT_PINS) ? OUTPUT_PINS[i].label : "None";
  };
  Serial.printf("Out config updated: ESTOP=%s UP=%s DOWN=%s LEFT=%s RIGHT=%s\n",
    pl(outIdxEstop), pl(outIdxUp), pl(outIdxDown), pl(outIdxLeft), pl(outIdxRight));
  if (reqType == "fetch") {
    webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    webServer.send(200, "application/json", "{\"ok\":1}");
    return;
  }
  webServer.sendHeader("Location", "/?tab=outconfig&saved=1");
  webServer.send(303);
}

// ============================================================
//  Motion status  (GET /motion/status — JSON, used by home page)
// ============================================================
static void handleMotionStatus() {
  if (!webAuth()) return;
  String bleUserName = "";
  if (authenticated && connectedUserEmail[0] != '\0') {
    bleUserName = String(connectedUserEmail);
    for (int i = 0; i < userCount; i++) {
      if (String(users[i].email).equalsIgnoreCase(connectedUserEmail)) {
        bleUserName = String(users[i].name);
        break;
      }
    }
    bleUserName.replace("\"", "\\\"");
  }
  // estop in web JSON reflects the actual output line state if assigned.
  int estopOutputOn = (outIdxEstop < NUM_OUTPUT_PINS) ? (motionEstop ? 0 : 1) : 0;
  int upOutputOn     = (outIdxUp    < NUM_OUTPUT_PINS) ? (motionUp    ? 1 : 0) : 0;
  int downOutputOn   = (outIdxDown  < NUM_OUTPUT_PINS) ? (motionDown  ? 1 : 0) : 0;
  int leftOutputOn   = (outIdxLeft  < NUM_OUTPUT_PINS) ? (motionLeft  ? 1 : 0) : 0;
  int rightOutputOn  = (outIdxRight < NUM_OUTPUT_PINS) ? (motionRight ? 1 : 0) : 0;
  String json = "{\"estop\":" + String(estopOutputOn) +
                ",\"up\":"    + String(upOutputOn) +
                ",\"down\":"  + String(downOutputOn) +
                ",\"left\":"  + String(leftOutputOn) +
                ",\"right\":" + String(rightOutputOn) +
                ",\"ble_conn\":" + String(deviceConnected ? 1 : 0) +
                ",\"ble_auth\":" + String(authenticated  ? 1 : 0) +
                ",\"ble_user\":\"" + bleUserName + "\"}";
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  webServer.send(200, "application/json", json);
}

// ============================================================
//  Static CSS  (GET /style.css — cached by browser for 24 h)
// ============================================================
static void handleStyleCSS() {
  if (webServer.header("If-None-Match") == "\"v1\"") {
    webServer.sendHeader("Cache-Control", "public, max-age=86400");
    webServer.sendHeader("ETag", "\"v1\"");
    webServer.send(304);
    return;
  }
  webServer.sendHeader("Cache-Control", "public, max-age=86400");
  webServer.sendHeader("ETag", "\"v1\"");
  webServer.send(200, "text/css", HTML_STYLE);
}

void setupWebRoutes() {
  loadWebAdmin();
  const char* hdrs[] = {"Cookie", "If-None-Match", "X-Requested-With"};
  webServer.collectHeaders(hdrs, 3);
  webServer.on("/style.css",      HTTP_GET,  handleStyleCSS);
  webServer.on("/login",          HTTP_GET,  handleLoginGet);
  webServer.on("/login",          HTTP_POST, handleLoginPost);
  webServer.on("/logout",         HTTP_GET,  handleLogout);
  webServer.on("/",               HTTP_GET,  handleRoot);
  webServer.on("/add",            HTTP_GET,  handleAddGet);
  webServer.on("/add",            HTTP_POST, handleAddPost);
  webServer.on("/edit",           HTTP_GET,  handleEditGet);
  webServer.on("/edit",           HTTP_POST, handleEditPost);
  webServer.on("/confirm-delete", HTTP_GET,  handleConfirmDelete);
  webServer.on("/delete",         HTTP_POST, handleDeletePost);
  webServer.on("/account",        HTTP_GET,  handleAccountGet);
  webServer.on("/account",        HTTP_POST, handleAccountPost);
  webServer.on("/login-as",       HTTP_GET,  handleLoginAs);
  webServer.on("/motion/status",  HTTP_GET,  handleMotionStatus);
  webServer.on("/outconfig",      HTTP_POST, handleOutConfigPost);
  webServer.on("/netconfig",      HTTP_POST, handleNetConfigPost);
  webServer.begin();
  Serial.println("Web server started");
}
