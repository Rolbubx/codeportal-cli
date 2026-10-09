// providers.hpp : table des fournisseurs d'IA, construction des requêtes, streaming, outils natifs.
//
// Trois protocoles suffisent pour tous les fournisseurs :
//   - Gemini     : API Google Generative Language
//   - Anthropic  : API Messages d'Anthropic
//   - OpenAI     : chat/completions (OpenAI et tous les services compatibles : Poolside, Mistral, Groq...)
#pragma once

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "http.hpp"
#include "json.hpp"
#include "state.hpp"
#include "toolspec.hpp"
#include "util.hpp"

enum class Protocol { Gemini, Anthropic, OpenAI };

struct Provider {
    std::string id;            // identifiant utilisé dans les commandes
    std::string label;         // nom affiché
    std::string host;          // hôte HTTPS
    std::string chatPath;      // chemin de génération ({model} est remplacé)
    std::string modelsPath;    // chemin de la liste des modèles (vérification de clé)
    std::string defaultModel;
    Protocol protocol;
    std::vector<std::string> envVars;      // variables d'environnement acceptées
    std::vector<std::string> keyPrefixes;  // préfixes reconnaissables des clés
};

const std::vector<Provider>& providers() {
    static const std::vector<Provider> list = {
        {"gemini", "Gemini", "generativelanguage.googleapis.com", "/v1beta/models/{model}:generateContent",
         "/v1beta/models", "gemini-3.8-flash", Protocol::Gemini, {"GEMINI_API_KEY", "GOOGLE_API_KEY"}, {"AIza"}},
        {"anthropic", "Anthropic", "api.anthropic.com", "/v1/messages", "/v1/models", "claude-haiku-5-5",
         Protocol::Anthropic, {"ANTHROPIC_API_KEY"}, {"sk-ant-"}},
        {"openai", "OpenAI", "api.openai.com", "/v1/chat/completions", "/v1/models", "gpt-4o-mini",
         Protocol::OpenAI, {"OPENAI_API_KEY"}, {"sk-"}},
        {"poolside", "Poolside", "inference.poolside.ai", "/v1/chat/completions", "/v1/models",
         "poolside/laguna-s-2.1", Protocol::OpenAI, {"POOLSIDE_API_KEY"}, {}},
        {"mistral", "Mistral", "api.mistral.ai", "/v1/chat/completions", "/v1/models", "mistral-small-latest",
         Protocol::OpenAI, {"MISTRAL_API_KEY"}, {}},
        {"groq", "Groq", "api.groq.com", "/openai/v1/chat/completions", "/openai/v1/models",
         "llama-3.3-70b-versatile", Protocol::OpenAI, {"GROQ_API_KEY"}, {"gsk_"}},
        {"xai", "xAI", "api.x.ai", "/v1/chat/completions", "/v1/models", "grok-3-mini", Protocol::OpenAI,
         {"XAI_API_KEY"}, {"xai-"}},
        {"deepseek", "DeepSeek", "api.deepseek.com", "/chat/completions", "/models", "deepseek-chat",
         Protocol::OpenAI, {"DEEPSEEK_API_KEY"}, {"sk-"}},
        {"openrouter", "OpenRouter", "openrouter.ai", "/api/v1/chat/completions", "/api/v1/models",
         "openai/gpt-4o-mini", Protocol::OpenAI, {"OPENROUTER_API_KEY"}, {"sk-or-"}},
        {"cerebras", "Cerebras", "api.cerebras.ai", "/v1/chat/completions", "/v1/models", "llama3.1-8b",
         Protocol::OpenAI, {"CEREBRAS_API_KEY"}, {"csk-"}},
    };
    return list;
} // le meuilleur est Claude not gonna lie

const Provider* findProvider(const std::string& id) {
    for (const auto& p : providers())
        if (p.id == id) return &p;
    return nullptr;
}

// Priorité : clé enregistrée via /key, sinon variable d'environnement.
std::string keyFor(const Provider& p, std::string* source = nullptr) {
    auto it = g.storedKeys.find(p.id);
    if (it != g.storedKeys.end() && !it->second.empty()) {
        if (source) *source = "saved";
        return it->second;
    }
    for (const auto& var : p.envVars) {
        const char* v = std::getenv(var.c_str());
        if (v && *v) {
            if (source) *source = var;
            return v;
        }
    }
    return "";
}

std::string modelFor(const Provider& p) {
    auto it = g.models.find(p.id);
    return it != g.models.end() ? it->second : p.defaultModel;
}

const Provider* activeProvider() {
    if (const Provider* a = findProvider(g.activeId))
        if (!keyFor(*a).empty()) return a;
    for (const auto& p : providers())
        if (!keyFor(p).empty()) return &p;
    return nullptr;
}

std::string maskKey(const std::string& k) {
    if (k.size() <= 8) return "****";
    return "****" + k.substr(k.size() - 4);
}

// Fournisseurs dont un préfixe correspond à la clé (le plus long préfixe gagne).
std::vector<const Provider*> matchProviders(const std::string& key) {
    size_t best = 0;
    std::vector<const Provider*> out;
    for (const auto& p : providers()) {
        size_t len = 0;
        for (const auto& pre : p.keyPrefixes)
            if (key.rfind(pre, 0) == 0) len = std::max(len, pre.size());
        if (len == 0) continue;
        if (len > best) {
            best = len;
            out.clear();
        }
        if (len == best) out.push_back(&p);
    }
    return out;
}

std::vector<std::string> authHeaders(const Provider& p, const std::string& key) {
    switch (p.protocol) {
        case Protocol::Gemini: return {"x-goog-api-key: " + key};
        case Protocol::Anthropic: return {"x-api-key: " + key, "anthropic-version: 2023-06-01"};
        default: return {"Authorization: Bearer " + key};
    }
}

HttpResult providerRequest(const Provider& p, const std::string& method, const std::string& path,
                           const std::string& key, const std::string& body,
                           const std::function<bool(const std::string&)>& onData = nullptr) {
    std::vector<std::string> headers = authHeaders(p, key);
    if (!body.empty()) headers.push_back("Content-Type: application/json");
    return httpsStream(method, p.host, path, headers, body, onData);
}

std::string errorMessage(const JVal& root) {
    if (root.type == JVal::ARR && !root.arr.empty()) return errorMessage(root.arr[0]);  // Gemini en flux
    const JVal* e = root.find("error");
    if (!e) return "";
    if (e->type == JVal::STR) return e->str;
    const JVal* m = e->find("message");
    return (m && m->type == JVal::STR) ? m->str : "";
}

// Arguments d'outil normalisés en objet JSON valide ("{}" si illisibles).
std::string argsObject(const std::string& raw) {
    JVal v;
    JsonParser parser(raw.empty() ? "{}" : raw);
    if (parser.parse(v) && v.type == JVal::OBJ) return toJson(v);
    return "{}";
}

// ---------------------------------------------------------------------------
// Corps de requête : historique (messages, appels d'outils, résultats) + déclaration des outils
// ---------------------------------------------------------------------------
std::string toolsJson(Protocol protocol) {
    std::string out;
    for (const auto& t : toolSpecs()) {
        if (!out.empty()) out += ",";
        const std::string name = jsonStr(t.name), desc = jsonStr(t.desc);
        if (protocol == Protocol::OpenAI)
            out += "{\"type\":\"function\",\"function\":{\"name\":" + name + ",\"description\":" + desc +
                   ",\"parameters\":" + toolSchema(t) + "}}";
        else if (protocol == Protocol::Anthropic)
            out += "{\"name\":" + name + ",\"description\":" + desc + ",\"input_schema\":" + toolSchema(t) + "}";
        else  // Gemini refuse un objet sans propriétés : on omet alors "parameters"
            out += "{\"name\":" + name + ",\"description\":" + desc +
                   (t.params.empty() ? std::string() : ",\"parameters\":" + toolSchema(t)) + "}";
    }
    if (protocol == Protocol::Gemini) return "[{\"functionDeclarations\":[" + out + "]}]";
    return "[" + out + "]";
}

std::string openAiMessages(const std::string& system, const std::vector<Message>& msgs) {
    std::string b = "[{\"role\":\"system\",\"content\":" + jsonStr(system) + "}";
    for (const auto& m : msgs) {
        if (m.role == "tool") {
            b += ",{\"role\":\"tool\",\"tool_call_id\":" + jsonStr(m.toolCallId) + ",\"content\":" + jsonStr(m.text) + "}";
        } else if (m.role == "assistant" && !m.toolCalls.empty()) {
            b += ",{\"role\":\"assistant\",\"content\":" + (m.text.empty() ? std::string("null") : jsonStr(m.text)) +
                 ",\"tool_calls\":[";
            for (size_t i = 0; i < m.toolCalls.size(); ++i) {
                const auto& tc = m.toolCalls[i];
                b += std::string(i ? "," : "") + "{\"id\":" + jsonStr(tc.id) + ",\"type\":\"function\",\"function\":{" +
                     "\"name\":" + jsonStr(tc.name) + ",\"arguments\":" + jsonStr(argsObject(tc.args)) + "}}";
            }
            b += "]}";
        } else {
            b += ",{\"role\":" + jsonStr(m.role) + ",\"content\":" + jsonStr(m.text) + "}";
        }
    }
    return b + "]";
}

std::string anthropicMessages(const std::vector<Message>& msgs) {
    std::string b = "[";
    bool first = true;
    auto add = [&](const std::string& item) {
        b += (first ? "" : ",") + item;
        first = false;
    };
    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto& m = msgs[i];
        if (m.role == "tool") {  // résultats consécutifs regroupés dans un seul message utilisateur
            std::string blocks;
            for (; i < msgs.size() && msgs[i].role == "tool"; ++i)
                blocks += std::string(blocks.empty() ? "" : ",") + "{\"type\":\"tool_result\",\"tool_use_id\":" +
                          jsonStr(msgs[i].toolCallId) + ",\"content\":" + jsonStr(msgs[i].text) + "}";
            --i;
            add("{\"role\":\"user\",\"content\":[" + blocks + "]}");
        } else if (m.role == "assistant") {
            std::string blocks;
            auto block = [&](const std::string& s) { blocks += (blocks.empty() ? "" : ",") + s; };
            if (!m.thinking.empty() && !m.thinkingSignature.empty())
                block("{\"type\":\"thinking\",\"thinking\":" + jsonStr(m.thinking) + ",\"signature\":" +
                      jsonStr(m.thinkingSignature) + "}");
            if (!trim(m.text).empty()) block("{\"type\":\"text\",\"text\":" + jsonStr(m.text) + "}");
            for (const auto& tc : m.toolCalls)
                block("{\"type\":\"tool_use\",\"id\":" + jsonStr(tc.id) + ",\"name\":" + jsonStr(tc.name) +
                      ",\"input\":" + argsObject(tc.args) + "}");
            if (blocks.empty()) block("{\"type\":\"text\",\"text\":\"(no output)\"}");
            add("{\"role\":\"assistant\",\"content\":[" + blocks + "]}");
        } else {
            add("{\"role\":\"user\",\"content\":" + jsonStr(m.text) + "}");
        }
    }
    return b + "]";
}

std::string geminiContents(const std::vector<Message>& msgs) {
    std::string b = "[";
    bool first = true;
    auto add = [&](const std::string& item) {
        b += (first ? "" : ",") + item;
        first = false;
    };
    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto& m = msgs[i];
        if (m.role == "tool") {
            std::string parts;
            for (; i < msgs.size() && msgs[i].role == "tool"; ++i)
                parts += std::string(parts.empty() ? "" : ",") + "{\"functionResponse\":{\"name\":" +
                         jsonStr(msgs[i].toolName) + ",\"response\":{\"result\":" + jsonStr(msgs[i].text) + "}}}";
            --i;
            add("{\"role\":\"user\",\"parts\":[" + parts + "]}");
        } else if (m.role == "assistant") {
            std::string parts;
            auto part = [&](const std::string& s) { parts += (parts.empty() ? "" : ",") + s; };
            if (!trim(m.text).empty()) part("{\"text\":" + jsonStr(m.text) + "}");
            for (const auto& tc : m.toolCalls)
                part("{\"functionCall\":{\"name\":" + jsonStr(tc.name) + ",\"args\":" + argsObject(tc.args) + "}" +
                     (tc.signature.empty() ? std::string() : ",\"thoughtSignature\":" + jsonStr(tc.signature)) + "}");
            if (parts.empty()) part("{\"text\":\"(no output)\"}");
            add("{\"role\":\"model\",\"parts\":[" + parts + "]}");
        } else {
            add("{\"role\":\"user\",\"parts\":[{\"text\":" + jsonStr(m.text) + "}]}");
        }
    }
    return b + "]";
}

// stream : réponse en flux (SSE). thinking : raisonnement natif. tools : outils natifs déclarés.
std::string buildBody(const Provider& p, const std::string& model, const std::string& system,
                      const std::vector<Message>& msgs, bool stream = false, bool thinking = false,
                      bool tools = false) {
    if (p.protocol == Protocol::Gemini) {
        std::string b = "{\"systemInstruction\":{\"parts\":[{\"text\":" + jsonStr(system) + "}]},\"contents\":" +
                        geminiContents(msgs);
        if (tools) b += ",\"tools\":" + toolsJson(p.protocol);
        if (thinking) b += ",\"generationConfig\":{\"thinkingConfig\":{\"includeThoughts\":true}}";
        return b + "}";
    }
    if (p.protocol == Protocol::OpenAI) {
        std::string b = "{\"model\":" + jsonStr(model) + ",\"messages\":" + openAiMessages(system, msgs);
        if (tools) b += ",\"tools\":" + toolsJson(p.protocol);
        if (stream) b += ",\"stream\":true";
        return b + "}";
    }
    std::string b = "{\"model\":" + jsonStr(model) + ",\"max_tokens\":" + (thinking ? "16000" : "4096") +
                    ",\"system\":" + jsonStr(system) + ",\"messages\":" + anthropicMessages(msgs);
    if (tools) b += ",\"tools\":" + toolsJson(p.protocol);
    if (thinking) b += ",\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":4000}";
    if (stream) b += ",\"stream\":true";
    return b + "}";
}

std::string extractText(Protocol protocol, const JVal& root) {
    std::string out;
    if (protocol == Protocol::Gemini) {
        const JVal* cands = root.find("candidates");
        if (cands && cands->type == JVal::ARR && !cands->arr.empty()) {
            const JVal* content = cands->arr[0].find("content");
            const JVal* parts = content ? content->find("parts") : nullptr;
            if (parts && parts->type == JVal::ARR)
                for (const JVal& part : parts->arr)
                    if (const JVal* t = part.find("text")) out += t->str;
        }
    } else if (protocol == Protocol::OpenAI) {
        const JVal* choices = root.find("choices");
        if (choices && choices->type == JVal::ARR && !choices->arr.empty()) {
            const JVal* msg = choices->arr[0].find("message");
            const JVal* c = msg ? msg->find("content") : nullptr;
            if (c && c->type == JVal::STR) out = c->str;
        }
    } else {
        const JVal* content = root.find("content");
        if (content && content->type == JVal::ARR)
            for (const JVal& block : content->arr)
                if (const JVal* t = block.find("text")) out += t->str;
    }
    return out;
}

// Réponse complète, sans streaming ni outils (conservé pour des usages simples).
std::string callProvider(const Provider& p, const std::string& key, const std::string& model,
                         const std::string& system, const std::vector<Message>& msgs) {
    std::string path = p.chatPath;
    if (auto pos = path.find("{model}"); pos != std::string::npos) path.replace(pos, 7, model);

    HttpResult r = providerRequest(p, "POST", path, key, buildBody(p, model, system, msgs));

    JVal root;
    JsonParser parser(r.body);
    if (!parser.parse(root)) throw std::runtime_error("unreadable response (HTTP " + std::to_string(r.status) + ")");
    if (r.status != 200) {
        std::string msg = errorMessage(root);
        throw std::runtime_error("HTTP " + std::to_string(r.status) + (msg.empty() ? "" : ": " + msg));
    }
    std::string text = extractText(p.protocol, root);
    if (text.empty()) throw std::runtime_error("empty response");
    return text;
}

// ---------------------------------------------------------------------------
// Streaming : texte mot par mot, raisonnement natif, appels d'outils natifs.
// ---------------------------------------------------------------------------
struct StreamHandlers {
    std::function<void(const std::string&)> onThinking;  // raisonnement natif
    std::function<void(const std::string&)> onText;      // texte de la réponse
    std::function<void(size_t)> onTokens;                // tokens générés (si l'API les donne)
    std::function<bool()> cancelled;                     // vrai pour interrompre
};

struct StreamOutcome {
    std::string text;
    std::string reasoning;
    std::string reasoningSignature;  // Anthropic : signature du bloc de réflexion
    std::vector<ToolCallRec> toolCalls;
    bool cancelled = false;
};

// Server-Sent Events : lignes "data: {...}".
class SseReader {
public:
    template <class F>
    void feed(const std::string& chunk, F&& onData) {
        buf_ += chunk;
        size_t pos;
        while ((pos = buf_.find('\n')) != std::string::npos) {
            std::string line = buf_.substr(0, pos);
            buf_.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) != 0) continue;
            std::string data = trim(line.substr(5));
            if (!data.empty() && data != "[DONE]") onData(data);
        }
    }

private:
    std::string buf_;
};

size_t jsonCount(const JVal* v) {
    if (!v || v->type != JVal::NUM) return 0;
    try {
        return (size_t)std::stoull(v->str);
    } catch (...) {
        return 0;
    }
}

StreamOutcome streamProvider(const Provider& p, const std::string& key, const std::string& model,
                             const std::string& system, const std::vector<Message>& msgs, const StreamHandlers& h) {
    std::string path = p.chatPath;
    if (auto pos = path.find("{model}"); pos != std::string::npos) path.replace(pos, 7, model);
    if (p.protocol == Protocol::Gemini) replaceAll(path, ":generateContent", ":streamGenerateContent?alt=sse");

    // Les modèles compatibles OpenAI envoient leur raisonnement d'eux-mêmes (reasoning_content).
    bool thinking = p.protocol != Protocol::OpenAI;

    for (int attempt = 0; attempt < 2; ++attempt) {
        StreamOutcome out;
        SseReader sse;
        std::string streamError;
        std::map<size_t, ToolCallRec> calls;  // par index de bloc / d'appel

        auto think = [&](const std::string& t) {
            if (t.empty()) return;
            out.reasoning += t;
            if (h.onThinking) h.onThinking(t);
        };
        auto text = [&](const std::string& t) {
            if (t.empty()) return;
            out.text += t;
            if (h.onText) h.onText(t);
        };
        auto tokens = [&](size_t n) {
            if (n && h.onTokens) h.onTokens(n);
        };

        auto onEvent = [&](const std::string& data) {
            JVal ev;
            JsonParser parser(data);
            if (!parser.parse(ev)) return;
            if (p.protocol == Protocol::OpenAI) {
                const JVal* choices = ev.find("choices");
                if (choices && choices->type == JVal::ARR && !choices->arr.empty()) {
                    if (const JVal* d = choices->arr[0].find("delta")) {
                        for (const char* k : {"reasoning_content", "reasoning"}) {
                            const JVal* r = d->find(k);
                            if (r && r->type == JVal::STR) {
                                think(r->str);
                                break;
                            }
                        }
                        const JVal* c = d->find("content");
                        if (c && c->type == JVal::STR) text(c->str);
                        const JVal* tcs = d->find("tool_calls");
                        if (tcs && tcs->type == JVal::ARR) {
                            for (const JVal& tc : tcs->arr) {
                                ToolCallRec& rec = calls[jsonCount(tc.find("index"))];
                                const JVal* id = tc.find("id");
                                if (id && id->type == JVal::STR && !id->str.empty()) rec.id = id->str;
                                if (const JVal* fn = tc.find("function")) {
                                    const JVal* n = fn->find("name");
                                    if (n && n->type == JVal::STR && !n->str.empty()) rec.name = n->str;
                                    const JVal* a = fn->find("arguments");
                                    if (a && a->type == JVal::STR) rec.args += a->str;
                                }
                            }
                        }
                    }
                }
                if (const JVal* u = ev.find("usage")) tokens(jsonCount(u->find("completion_tokens")));
            } else if (p.protocol == Protocol::Gemini) {
                const JVal* cands = ev.find("candidates");
                if (cands && cands->type == JVal::ARR && !cands->arr.empty()) {
                    const JVal* content = cands->arr[0].find("content");
                    const JVal* parts = content ? content->find("parts") : nullptr;
                    if (parts && parts->type == JVal::ARR) {
                        for (const JVal& part : parts->arr) {
                            if (const JVal* fc = part.find("functionCall")) {
                                ToolCallRec rec;
                                if (const JVal* n = fc->find("name")) rec.name = n->str;
                                if (const JVal* a = fc->find("args")) rec.args = toJson(*a);
                                if (const JVal* s = part.find("thoughtSignature")) rec.signature = s->str;
                                calls[calls.size()] = rec;
                                continue;
                            }
                            const JVal* t = part.find("text");
                            if (!t) continue;
                            const JVal* thought = part.find("thought");
                            if (thought && thought->str == "true") think(t->str);
                            else text(t->str);
                        }
                    }
                }
                if (const JVal* u = ev.find("usageMetadata"))
                    tokens(jsonCount(u->find("candidatesTokenCount")) + jsonCount(u->find("thoughtsTokenCount")));
            } else {  // Anthropic
                const JVal* type = ev.find("type");
                const std::string t = type ? type->str : "";
                const size_t index = jsonCount(ev.find("index"));
                if (t == "content_block_start") {
                    const JVal* block = ev.find("content_block");
                    const JVal* bt = block ? block->find("type") : nullptr;
                    if (bt && bt->str == "tool_use") {
                        ToolCallRec& rec = calls[index];
                        if (const JVal* id = block->find("id")) rec.id = id->str;
                        if (const JVal* n = block->find("name")) rec.name = n->str;
                    }
                } else if (t == "content_block_delta") {
                    if (const JVal* d = ev.find("delta")) {
                        if (const JVal* x = d->find("thinking")) think(x->str);
                        if (const JVal* x = d->find("text")) text(x->str);
                        if (const JVal* x = d->find("signature")) out.reasoningSignature += x->str;
                        if (const JVal* x = d->find("partial_json")) calls[index].args += x->str;
                    }
                } else if (t == "message_delta") {
                    if (const JVal* u = ev.find("usage")) tokens(jsonCount(u->find("output_tokens")));
                } else if (t == "error") {
                    streamError = errorMessage(ev);
                }
            }
        };

        HttpResult r = providerRequest(p, "POST", path, key, buildBody(p, model, system, msgs, true, thinking, true),
                                       [&](const std::string& chunk) {
                                           sse.feed(chunk, onEvent);
                                           return !(h.cancelled && h.cancelled());
                                       });
        if (r.status != 200) {
            JVal root;
            JsonParser parser(r.body);
            std::string msg = parser.parse(root) ? errorMessage(root) : "";
            if (thinking && r.status == 400 && attempt == 0) {  // modèle sans raisonnement : on réessaie sans
                thinking = false;
                continue;
            }
            throw std::runtime_error("HTTP " + std::to_string(r.status) + (msg.empty() ? "" : ": " + msg));
        }
        if (!streamError.empty()) throw std::runtime_error(streamError);

        for (auto& [index, rec] : calls) {
            if (rec.name.empty()) continue;  // bloc de texte/réflexion Anthropic, pas un outil
            if (rec.id.empty()) rec.id = "call_" + std::to_string(index);
            rec.args = argsObject(rec.args);
            out.toolCalls.push_back(rec);
        }
        out.cancelled = r.cancelled;
        if (out.text.empty() && out.reasoning.empty() && out.toolCalls.empty() && !out.cancelled)
            throw std::runtime_error("empty response");
        return out;
    }
    throw std::runtime_error("request failed");
}

// ---------------------------------------------------------------------------
// Helpers pour le code : poser une question au modèle et récupérer sa réponse (texte brut).
// Rien n'est affiché, aucun outil n'est utilisé et l'historique de la conversation n'est pas modifié.
// En cas d'erreur (pas de clé, réseau, API), renvoient "" et décrivent l'erreur dans *error si fourni.
// ---------------------------------------------------------------------------

// Avec un fournisseur, une clé et un modèle donnés (utilisable depuis un autre fil).
std::string askModel(const Provider& p, const std::string& key, const std::string& model, const std::string& prompt,
                     const std::string& system = "", std::string* error = nullptr) {
    try {
        const std::string sys = system.empty() ? "You are a helpful assistant. Answer concisely." : system;
        return callProvider(p, key, model, sys, {Message("user", prompt)});
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return "";
    }
}

// Avec le fournisseur et le modèle actifs.
//   std::string err;
//   std::string resume = askAgent("Résume ce texte : " + texte, "", &err);
std::string askAgent(const std::string& prompt, const std::string& system = "", std::string* error = nullptr) {
    const Provider* p = activeProvider();
    if (!p) {
        if (error) *error = "No API key configured. Run /key first.";
        return "";
    }
    return askModel(*p, keyFor(*p), modelFor(*p), prompt, system, error);
}

// Vérifie une clé en listant les modèles. Retourne "" si la clé est acceptée.
std::string validateKey(const Provider& p, const std::string& key) {
    try {
        HttpResult r = providerRequest(p, "GET", p.modelsPath, key, "");
        if (r.status == 200) return "";
        JVal root;
        JsonParser parser(r.body);
        std::string msg = parser.parse(root) ? errorMessage(root) : "";
        return "HTTP " + std::to_string(r.status) + (msg.empty() ? "" : ": " + msg);
    } catch (const std::exception& e) {
        return e.what();
    }
}

// Garde seulement les modèles qui savent discuter (pas d'embedding, d'image, de voix...).
bool isChatModel(const JVal& item, const std::string& name) {
    // Gemini indique les méthodes prises en charge : il faut generateContent.
    if (const JVal* methods = item.find("supportedGenerationMethods")) {
        bool generates = false;
        for (const JVal& m : methods->arr)
            if (m.str == "generateContent") generates = true;
        if (!generates) return false;
    }
    std::string n = name;
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    for (const char* skip : {"embed", "whisper", "tts", "dall-e", "moderation", "transcribe", "image", "audio",
                             "realtime", "davinci", "babbage", "rerank", "guard", "speech", "ocr"})
        if (n.find(skip) != std::string::npos) return false;
    return true;
}

// Liste les modèles disponibles (lève une exception en cas d'erreur).
std::vector<std::string> listModels(const Provider& p, const std::string& key) {
    HttpResult r = providerRequest(p, "GET", p.modelsPath, key, "");
    JVal root;
    JsonParser parser(r.body);
    if (!parser.parse(root)) throw std::runtime_error("unreadable response (HTTP " + std::to_string(r.status) + ")");
    if (r.status != 200) {
        std::string msg = errorMessage(root);
        throw std::runtime_error("HTTP " + std::to_string(r.status) + (msg.empty() ? "" : ": " + msg));
    }
    std::vector<std::string> ids;
    for (const char* listKey : {"data", "models"}) {
        const JVal* arr = root.find(listKey);
        if (!arr || arr->type != JVal::ARR) continue;
        for (const JVal& item : arr->arr) {
            const JVal* id = item.find("id");
            if (!id) id = item.find("name");
            if (!id || id->type != JVal::STR) continue;
            std::string name = id->str;
            if (name.rfind("models/", 0) == 0) name = name.substr(7);
            if (!isChatModel(item, name)) continue;
            ids.push_back(name);
        }
    }
    return ids;
}
