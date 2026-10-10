#ifndef slic3r_ForcaAI_hpp_
#define slic3r_ForcaAI_hpp_

#include <nlohmann/json.hpp>

#include <atomic>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r { namespace GUI {

// Forca AI -- lets an AI (Claude Code or any MCP client) drive Forca, like Blender's AI connection.
//
// Forca hosts an MCP server itself ("streamable HTTP" transport: JSON-RPC 2.0 over HTTP POST) on 127.0.0.1 only,
// guarded by a secret key the user copies into their AI client. Every tool call runs on the GUI thread, so tools use
// Forca's normal code paths. Opt-in: nothing listens until the user turns Forca AI on.
//
// Aaron's hard rules (HQ PLAN_forca_ai.md) live below every tool, in Forca, not in AI instructions:
//   R1  never save over the user's files (AI saves get "(Claude <date>)" names; it may update only its own files)
//   R2  nothing is sent to a printer without the user's one-time, in-app approval
// Phase 2 (hands) edits the plate and presets and saves files through R1. Phase 3a: the AI may only request a print;
// the user approves it on a card in Forca (ForcaAIPrint.cpp), and no tool talks to a printer itself.

struct ForcaAIContent
{
    std::string type;  // "text" | "image"
    std::string text;  // for text
    std::string data;  // for image: base64
    std::string mime;  // for image
};

struct ForcaAIResult
{
    std::vector<ForcaAIContent> content;
    bool                        is_error = false;

    static ForcaAIResult text(const std::string& t);
    static ForcaAIResult json(const nlohmann::json& j); // pretty-printed JSON as text
    static ForcaAIResult error(const std::string& message);
    ForcaAIResult&       add_png(const std::string& png_bytes);
    ForcaAIResult&       add_image(const std::string& bytes, const std::string& mime); // e.g. "image/jpeg"
};

struct ForcaAITool
{
    std::string                                          name;
    std::string                                          title;
    std::string                                          description;
    nlohmann::json                                       input_schema; // JSON Schema object
    std::function<ForcaAIResult(const nlohmann::json&)>  run;          // called on the GUI thread...
    bool                                                 off_gui = false; // ...or on the server thread (slow I/O);
                                                                          // such a tool uses ForcaAI::on_gui for GUI state
};

struct ForcaAIActivity
{
    std::string time;    // HH:MM:SS
    std::string action;  // tool name or connection event
    std::string summary; // one line, human readable
    bool        ok = true;
};

class ForcaAI
{
public:
    enum class State { Off, Listening, Connected, Error };

    static ForcaAI& instance();

    // Settings (GUI thread).
    bool        enabled() const;          // opt-in; remembered in the app config
    void        set_enabled(bool on);     // persists + starts/stops the server
    void        start_if_enabled();       // app start
    void        shutdown();               // app exit
    int         port() const;
    std::string token();                  // the secret key (created on first use, kept in the data folder)
    void        regenerate_token();
    std::string url() const;
    std::string claude_code_command();    // ready to paste into a terminal

    // Control level (HQ PLAN_forca_ai.md §10a). Guarded, the default, is R1 + R2 as above. Advanced: the AI may also
    // overwrite the user's files and edit their presets (Forca backs each file up first, ForcaAIFiles), and a print
    // request opens Forca's print dialog without the approval card -- the user still presses Send. Only the user
    // changes it, in the Forca AI window; no tool can. Remembered in the app config.
    enum class Level { Guarded, Advanced };
    Level              level() const { return m_level; }
    void               set_level(Level l);        // GUI thread: persists, logs, notifies
    static std::string level_name(Level l);      // "guarded" | "advanced"

    State                        state() const { return m_state; }
    std::string                  state_text() const;
    std::vector<ForcaAIActivity> activity() const;

    // Called on the GUI thread whenever the state or the activity feed changes (the Forca AI window, the top bar).
    void add_listener(std::function<void()> fn);

    // For off_gui tools: runs fn on the GUI thread and waits (up to timeout_ms). False if it did not run in time.
    bool on_gui(std::function<void()> fn, int timeout_ms = 30000);

    // Adds a line to the activity feed + forca/ai/activity.jsonl (any thread).
    void log(const std::string& action, const std::string& summary, bool ok);

    void                              register_tool(ForcaAITool tool);
    const std::vector<ForcaAITool>&   tools() const { return m_tools; }

    // Server side (called on the server thread).
    struct HttpReply { int status = 200; std::string body; std::string content_type = "application/json";
                       std::map<std::string, std::string> headers; };
    HttpReply handle_http(const std::string& method, const std::string& target,
                          const std::map<std::string, std::string>& headers, const std::string& body);

private:
    ForcaAI();
    ~ForcaAI();
    void start();
    void stop();
    void run_server(int port);
    nlohmann::json handle_rpc(const nlohmann::json& req, bool& is_notification);
    ForcaAIResult  call_tool_on_gui(const ForcaAITool& tool, const nlohmann::json& args);
    void           set_state(State s, const std::string& error = {});
    void           notify();
    std::string    token_path() const;

    std::vector<ForcaAITool>            m_tools;
    std::vector<std::function<void()>>  m_listeners;
    mutable std::mutex                  m_mutex;         // m_activity, m_error
    std::deque<ForcaAIActivity>         m_activity;
    std::string                         m_error;
    std::string                         m_token;         // cached; read by the server thread under m_mutex
    std::atomic<State>                  m_state { State::Off };
    std::atomic<Level>                  m_level { Level::Guarded }; // read by off-GUI tools too
    std::atomic<bool>                   m_stopping { false };
    std::unique_ptr<std::thread>        m_thread;
    struct Server;
    std::shared_ptr<Server>             m_server;
};

// ForcaAITools.cpp: the Phase-1 (read-only) tools; it also registers the Phase-2 tools.
// DNS-rebinding guard for the bridge: a browser's Origin header, or the request's Host header, must name this computer
// exactly (localhost, 127.0.0.1 or [::1], any port). A prefix match would let "localhost.evil.com" through.
bool forca_ai_is_local_origin(const std::string& origin); // "http://localhost:1234"
bool forca_ai_is_local_host(const std::string& host);     // "127.0.0.1:13630"

void register_forca_ai_tools(ForcaAI& ai);
// ForcaAIHands.cpp: the Phase-2 tools (models, presets, settings, modifiers, slicing, saving).
void register_forca_ai_hand_tools(ForcaAI& ai);
// The slice report of a sliced plate (0-based): time, filament, layers, warnings. Empty + `err` if not sliced.
nlohmann::json forca_ai_plate_report(int plate, std::string& err);
// ForcaAIPrint.cpp: R2 -- the AI may only REQUEST a print; the user approves it in Forca.
void register_forca_ai_print_tools(ForcaAI& ai);
// ForcaAIAcademy.cpp: Forca Academy (print journal / knowledge base) tools.
void register_forca_ai_academy_tools(ForcaAI& ai);
// ForcaAIPrinters.cpp: per-printer opt-in (the AI sees only printers the user ticked) + read-only status tools.
std::set<std::string>    forca_ai_printer_optins();
void                     forca_ai_set_printer_optin(const std::string& dev_id, bool on);
std::set<std::string>    forca_ai_pause_optins(); // "AI may pause" (counts only on a shared printer)
void                     forca_ai_set_pause_optin(const std::string& dev_id, bool on);
std::vector<std::string> forca_ai_known_printers(); // device ids Forca knows (LAN + account)
std::string              forca_ai_printer_label(const std::string& dev_id);
// One camera frame (JPEG) of a Bambu printer Forca knows, not gated by AI sharing (Forca Academy's own pictures).
// BLOCKS up to ~25 s: call from a worker thread, never the GUI thread.
bool                     forca_printer_camera_jpeg(const std::string& dev_id, int max_width, std::string& jpeg, std::string& err);
void                     register_forca_ai_printer_tools(ForcaAI& ai);

// ForcaAIAutonomy.cpp: pre-approved autonomy (HQ PLAN_forca_ai.md §10a) -- a print grant the user gives in the Forca
// AI window, at the Advanced level only. Kept in memory: a restart, the Guarded level, expiry or Revoke end it.
// One file a grant asks to print: the AI imports it, arranges and slices copies, and Forca sends only plates made
// of grant files, up to the copies still owed.
struct ForcaAIGrantFile
{
    std::string path;       // absolute, UTF-8
    int         copies = 1;
    int         done   = 0; // copies Forca has sent
    std::string note;       // the user's instructions for the AI (orientation, material, settings...)
};
struct ForcaAIGrant
{
    int                   id            = 0;     // 0 = no grant
    std::set<std::string> printers;              // device ids, each shared with the AI
    int                   prints_left   = 0;
    std::time_t           expires       = 0;
    int                   max_print_min = 0;     // the longest print the AI may start
    bool                  camera_check  = false; // the AI may clear a bed with its own camera check (forca_bed_check)
    std::vector<ForcaAIGrantFile> files;         // what to print; empty = any plate
};
ForcaAIGrant   forca_ai_grant();                               // the active grant; id 0 when none, expired or used up
void           forca_ai_give_grant(ForcaAIGrant g);            // the user's grant (replaces any)
void           forca_ai_revoke_grant(const std::string& why);
std::string    forca_ai_mark_bed_clear(const std::string& dev_id); // the user's "Bed is clear"; else why not
bool           forca_ai_bed_is_clear(const std::string& dev_id);
nlohmann::json forca_ai_grant_json();                          // for forca_status
// A grant print: forca_ai_grant_check says whether the AI may start it now (empty = yes, `dev_id` = the printer);
// forca_ai_grant_send then opens Forca's print dialog, waits until it is ready with nothing to confirm, counts down
// 10 s visibly (the user can cancel), checks everything again and presses Send. It returns when the dialog closes.
struct ForcaAIGrantPrint
{
    int                                                                       request      = 0;
    int                                                                       plate        = 0; // 0-based
    std::string                                                               dev_id;
    int                                                                       print_time_s = 0;
    int                                                                       bed_check    = 0;
    std::map<int, std::string>                                                trays; // project filament slot (1-based) -> printer slot ("A4", "Ext")
    std::vector<int>                                                          used;  // the project filament slots the plate prints with
    std::function<std::string()>                                              stale; // why the slice no longer matches, or empty
    std::function<void(const std::string& state, const std::string& detail)> done;  // "sent" | "void" | "cancelled"
};
std::string forca_ai_grant_check(const ForcaAIGrantPrint& job, const std::string& printer, std::string& dev_id);
// A grant with files: every object on the plate must come from one of them (`objects`: each object's source file and
// its instances on the plate), and no file may get more copies than it still owes. Empty = allowed, with `count[i]`
// the copies of files[i] on the plate; else why not.
std::string forca_ai_grant_files_reason(const std::vector<ForcaAIGrantFile>& files,
                                        const std::vector<std::pair<std::string, int>>& objects, std::vector<int>& count);
// A printer slot name as Forca shows it ("A4", "B1", "HT-A", "Ext", "Ext 2", any case) -> the printer's ams_id / slot_id.
bool        forca_ai_parse_tray(const std::string& label, std::string& ams_id, std::string& slot_id);
void        forca_ai_grant_send(ForcaAIGrantPrint job);
void        register_forca_ai_autonomy_tools(ForcaAI& ai);

}} // namespace Slic3r::GUI

#endif // slic3r_ForcaAI_hpp_
