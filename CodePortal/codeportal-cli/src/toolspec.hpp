// toolspec.hpp : description des outils natifs envoyée aux API (function calling).
// Le modèle les appelle directement ; le programme les exécute (voir tools.hpp / agent.hpp).
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

struct ToolParam {
    const char* name;
    const char* type;  // "string" ou "integer"
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
        {"read_file", "Read a text file. Returns its lines, numbered.",
         {{"path", "string", "File path (absolute, or relative to the working directory)", true}}},
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
