// toolspec.hpp : description des outils natifs envoyée aux API (function calling).
// Le modèle les appelle directement ; le programme les exécute (voir tools.hpp / agent.hpp).
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

struct ToolParam {
    const char* name;
    const char* type;  // "string", "integer" ou "boolean"
    const char* desc;
    bool required;
};

struct ToolSpec {
    const char* name;
    const char* desc;
    std::vector<ToolParam> params;
};

const std::vector<ToolSpec>& toolSpecs() {
    static const std::vector<ToolSpec> specs = {
        {"read_file",
         "Read a text file. Returns its lines, numbered (500 lines at a time; use start_line to read further).",
         {{"path", "string", "File path (absolute, or relative to the working directory)", true},
          {"start_line", "integer", "First line to read (default 1)", false},
          {"end_line", "integer", "Last line to read (default start_line + 499)", false}}},
        {"list_dir", "List the files and folders in a directory.",
         {{"path", "string", "Directory path (default: working directory)", false}}},
        {"create_file", "Create a new file with the given content. Fails if the file already exists.",
         {{"path", "string", "Path of the new file", true}, {"content", "string", "Full file content", true}}},
        {"write_file", "Create or completely overwrite a file with the given content.",
         {{"path", "string", "File path", true}, {"content", "string", "Full new file content", true}}},
        {"edit_file",
         "Replace an exact piece of text in an existing file. old_text must appear exactly once (same spaces and "
         "line breaks). Read the file first.",
         {{"path", "string", "File path", true},
          {"old_text", "string", "Exact text to replace", true},
          {"new_text", "string", "Replacement text", true}}},
        {"delete_file", "Delete a file.", {{"path", "string", "File path", true}}},
        {"create_dir", "Create a directory, including missing parent directories.",
         {{"path", "string", "Directory path", true}}},
        {"run_command",
         "Run a Windows cmd.exe command and return its output. It runs without keyboard input: never start "
         "interactive programs. Prefer the file tools for creating, editing or deleting files.",
         {{"command", "string", "The command line, e.g. dir /b or git status", true},
          {"timeout_seconds", "integer", "Maximum run time in seconds (default 120, max 600)", false}}},
        {"get_context", "Return the conversation context (messages so far, provider, working directory).", {}},

        // --- explorer et chercher ---
        {"tree", "Show a folder as an indented tree (skips .git, node_modules and similar).",
         {{"path", "string", "Folder (default: working directory)", false},
          {"depth", "integer", "How many levels to show (default 3)", false},
          {"max_entries", "integer", "Maximum entries (default 400)", false}}},
        {"find_files", "Find files by name pattern, recursively. Returns relative paths.",
         {{"pattern", "string", "Wildcards like *.cpp, test_*.py, src/*.hpp; several separated by ;", true},
          {"path", "string", "Folder to search (default: working directory)", false},
          {"max_results", "integer", "Maximum results (default 200)", false}}},
        {"search_files",
         "Search the text of files (like grep). Returns file:line: text for each match. Faster and safer than "
         "findstr through run_command.",
         {{"query", "string", "Text to find (or a regular expression if regex is true)", true},
          {"path", "string", "Folder or file to search (default: working directory)", false},
          {"glob", "string", "Only files matching this pattern, e.g. *.cpp;*.hpp (default: all)", false},
          {"regex", "boolean", "Treat query as an ECMAScript regular expression (default false)", false},
          {"case_sensitive", "boolean", "Match case (default false)", false},
          {"max_results", "integer", "Maximum matches (default 100)", false}}},
        {"file_info", "Information about a file or folder: exists, size, line count, line endings, dates.",
         {{"path", "string", "File or folder path", true}}},

        // --- modifier des fichiers ---
        {"replace_lines",
         "Replace a range of lines (start_line to end_line, inclusive) with new text. Use it when edit_file cannot "
         "match the text exactly. Empty new_text deletes the lines. Read the file first to get line numbers.",
         {{"path", "string", "File path", true},
          {"start_line", "integer", "First line to replace", true},
          {"end_line", "integer", "Last line to replace (default: start_line)", false},
          {"new_text", "string", "Replacement text (may span several lines)", true}}},
        {"insert_lines", "Insert text before a given line. Use line = line count + 1 to add at the end.",
         {{"path", "string", "File path", true},
          {"line", "integer", "The new text goes before this line", true},
          {"text", "string", "Text to insert (may span several lines)", true}}},
        {"append_file", "Add text at the end of a file (creates it if needed).",
         {{"path", "string", "File path", true}, {"content", "string", "Text to add", true}}},
        {"copy_path", "Copy a file or a folder (recursively).",
         {{"source", "string", "File or folder to copy", true},
          {"destination", "string", "New path (or an existing folder to copy into)", true},
          {"overwrite", "boolean", "Replace an existing destination (default false)", false}}},
        {"move_path", "Move or rename a file or folder.",
         {{"source", "string", "File or folder to move", true},
          {"destination", "string", "New path (or an existing folder to move into)", true},
          {"overwrite", "boolean", "Replace an existing destination (default false)", false}}},
        {"delete_dir",
         "Delete a folder. Without recursive it must be empty. This cannot be undone. Drive roots, user folders "
         "and the working folder are refused.",
         {{"path", "string", "Folder path", true},
          {"recursive", "boolean", "Also delete everything inside (default false)", false}}},
        {"undo_last_change", "Undo the last file change made by a tool (restores the previous content).", {}},

        // --- programmes ---
        {"run_powershell",
         "Run a PowerShell script and return its output. Prefer it over run_command for anything beyond a simple "
         "command: no cmd.exe quoting problems, full .NET access. No keyboard input.",
         {{"script", "string", "PowerShell code (several lines allowed)", true},
          {"timeout_seconds", "integer", "Maximum run time in seconds (default 120, max 600)", false}}},
        {"start_process",
         "Start a long-running program in the background without waiting (a dev server, a watcher, an app). "
         "Output goes to a log file you can read with read_file.",
         {{"command", "string", "cmd.exe command line", true},
          {"working_dir", "string", "Folder to run it in (default: working directory)", false},
          {"visible", "boolean", "Open it in its own console window instead of logging (default false)", false}}},
        {"stop_process", "Stop a process and its children.", {{"pid", "integer", "Process ID", true}}},
        {"list_processes", "List running processes (PID, memory, name), biggest first.",
         {{"filter", "string", "Only names containing this text", false},
          {"max_results", "integer", "Maximum results (default 60)", false}}},
        {"wait", "Wait a few seconds (e.g. for a server you started to be ready).",
         {{"seconds", "integer", "Seconds to wait (1 to 300)", true}}},

        // --- web ---
        {"fetch_url",
         "Download a web page or API response and return it as text (HTML is converted to readable text).",
         {{"url", "string", "http:// or https:// URL", true},
          {"max_chars", "integer", "Maximum characters returned (default 20000)", false},
          {"raw", "boolean", "Return HTML as-is instead of converting it to text (default false)", false}}},
        {"download_file", "Download a file from the web and save it to disk.",
         {{"url", "string", "http:// or https:// URL", true},
          {"path", "string", "Where to save it (a file path, or a folder)", true},
          {"overwrite", "boolean", "Replace an existing file (default false)", false}}},
        {"web_search", "Search the web. Returns titles, URLs and snippets; read pages with fetch_url.",
         {{"query", "string", "Search terms", true},
          {"max_results", "integer", "Maximum results (default 8, max 20)", false}}},

        // --- Windows ---
        {"open_path", "Open a file, folder or URL with its default app on the user's PC (Explorer, browser...).",
         {{"target", "string", "File path, folder path or URL", true}}},
        {"clipboard_read", "Read the text currently in the Windows clipboard.", {}},
        {"clipboard_write", "Put text in the Windows clipboard.", {{"text", "string", "Text to copy", true}}},
        {"system_info", "Windows version, computer name, CPU, memory, disk space, uptime and local time.", {}},
        {"get_env",
         "Read an environment variable. Without a name, list variable names. Values that look like secrets are "
         "never shown.",
         {{"name", "string", "Variable name, e.g. PATH or JAVA_HOME", false}}},

        // --- agent ---
        {"calculate",
         "Evaluate a math expression exactly: + - * / % ^, parentheses, sqrt, sin, cos, log, ln, exp, min, max, "
         "round, pi, e...",
         {{"expression", "string", "The expression, e.g. (17.5 * 3) ^ 2 / sqrt(2)", true}}},
        {"update_plan",
         "Show the user your step-by-step plan as a checklist, and update it as you progress. Use it for tasks with "
         "3 or more steps.",
         {{"items", "string", "One step per line: [x] done, [~] in progress, [ ] to do", true}}},
        {"save_memory",
         "Remember a short, lasting fact for future conversations (a user preference, a project detail they asked "
         "you to remember). Saved notes are given to you at the start of every conversation.",
         {{"note", "string", "One short sentence", true}}},
    };
    return specs;
}

// Schéma JSON des paramètres : {"type":"object","properties":{...},"required":[...]}
std::string toolSchema(const ToolSpec& t) {
    std::string props, req;
    for (const auto& p : t.params) {
        if (!props.empty()) props += ",";
        props += jsonStr(p.name) + ":{\"type\":" + jsonStr(p.type) + ",\"description\":" + jsonStr(p.desc) + "}";
        if (p.required) req += std::string(req.empty() ? "" : ",") + jsonStr(p.name);
    }
    return "{\"type\":\"object\",\"properties\":{" + props + "},\"required\":[" + req + "]}";
}
