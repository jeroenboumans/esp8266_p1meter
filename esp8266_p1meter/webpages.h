// **********************************
// * Web interface templates        *
// **********************************
//
// * Kept in PROGMEM so these strings live in flash instead of eating up the
// * little RAM the esp8266 has left.

static const char PAGE_HEADER[] PROGMEM =
    "<!DOCTYPE html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>P1 meter</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,sans-serif;margin:0;padding:1.5rem;"
    "background:#f4f5f7;color:#1c1e21;line-height:1.5}"
    "main{max-width:34rem;margin:0 auto}"
    "h1{font-size:1.4rem;margin:0 0 .25rem}"
    "h2{font-size:1rem;margin:1.5rem 0 .5rem;color:#5a6270}"
    "nav{margin:.5rem 0 1.25rem}"
    "nav a{margin-right:1rem;color:#0b62d0;text-decoration:none}"
    "nav a:hover{text-decoration:underline}"
    ".card{background:#fff;border:1px solid #dfe3e8;border-radius:8px;"
    "padding:1rem 1.25rem;margin-bottom:1rem}"
    "table{width:100%;border-collapse:collapse}"
    "td{padding:.3rem 0;vertical-align:top;font-size:.92rem}"
    "td:first-child{color:#5a6270;width:52%}"
    "td:last-child{text-align:right;font-variant-numeric:tabular-nums}"
    "label{display:block;margin:.75rem 0 .2rem;font-size:.9rem;color:#5a6270}"
    "input{width:100%;box-sizing:border-box;padding:.5rem;border:1px solid #c4cad2;"
    "border-radius:6px;font-size:1rem;background:#fff;color:#1c1e21}"
    "button{margin-top:1rem;padding:.55rem 1.1rem;border:0;border-radius:6px;"
    "background:#0b62d0;color:#fff;font-size:.95rem;cursor:pointer}"
    "button:hover{background:#0951ae}"
    "button.secondary{background:#5a6270}button.secondary:hover{background:#454c58}"
    "form.inline{display:inline}"
    ".ok{color:#1a7f37;font-weight:600}.bad{color:#b3261e;font-weight:600}"
    ".note{font-size:.85rem;color:#5a6270;margin-top:.75rem}"
    ".warn{background:#fff6e5;border-color:#e8c37a}"
    "</style></head><body><main>"
    "<h1>P1 meter</h1>"
    "<nav><a href=\"/\">Status</a><a href=\"/config\">Settings</a></nav>";

static const char PAGE_FOOTER[] PROGMEM =
    "</main></body></html>";
