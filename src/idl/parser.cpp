// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// src/idl/parser.cpp — OMG IDL lexer and recursive-descent parser.
//
// C++ port of go-DDS's tools/idl/parser.go, faithfully mirroring its
// token set, grammar coverage, and quirks (including the parenthesized-
// annotation-argument limitation documented in dds/idl/idl.hpp).

#include "dds/idl/idl.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace dds::idl {
namespace {

// ── Lexer ─────────────────────────────────────────────────────────────────────

enum class TokKind {
    eof,
    ident,        // identifier or keyword
    number,       // integer literal (used for array sizes)
    lbrace,       // {
    rbrace,       // }
    langle,       // <
    rangle,       // >
    lbracket,     // [
    rbracket,     // ]
    semi,         // ;
    comma,        // ,
    double_colon, // ::
    at,           // @
};

struct Token {
    TokKind kind{TokKind::eof};
    std::string val;
    int line{1};
};

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}
bool is_ident_cont(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src) {}

    Token next() {
        skip_whitespace_and_comments();
        if (pos_ >= src_.size()) {
            return Token{TokKind::eof, "", line_};
        }
        char c = src_[pos_];
        int ln = line_;
        switch (c) {
            case '{': advance(); return Token{TokKind::lbrace, "{", ln};
            case '}': advance(); return Token{TokKind::rbrace, "}", ln};
            case '<': advance(); return Token{TokKind::langle, "<", ln};
            case '>': advance(); return Token{TokKind::rangle, ">", ln};
            case '[': advance(); return Token{TokKind::lbracket, "[", ln};
            case ']': advance(); return Token{TokKind::rbracket, "]", ln};
            case ';': advance(); return Token{TokKind::semi, ";", ln};
            case ',': advance(); return Token{TokKind::comma, ",", ln};
            case '@': advance(); return Token{TokKind::at, "@", ln};
            case ':':
                if (pos_ + 1 < src_.size() && src_[pos_ + 1] == ':') {
                    advance();
                    advance();
                    return Token{TokKind::double_colon, "::", ln};
                }
                // single colon -- skip
                advance();
                return next();
            default:
                break;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            std::string b;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
                b.push_back(src_[pos_]);
                ++pos_;
            }
            return Token{TokKind::number, b, ln};
        }
        if (is_ident_start(c)) {
            std::string b;
            while (pos_ < src_.size() && is_ident_cont(src_[pos_])) {
                b.push_back(src_[pos_]);
                ++pos_;
            }
            return Token{TokKind::ident, b, ln};
        }
        // Unknown character (including '(' and ')') -- skip.
        advance();
        return next();
    }

private:
    void advance() {
        if (src_[pos_] == '\n') {
            ++line_;
        }
        ++pos_;
    }

    void skip_whitespace_and_comments() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                advance();
                continue;
            }
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') {
                    ++pos_;
                }
                continue;
            }
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < src_.size()) {
                    if (src_[pos_] == '*' && src_[pos_ + 1] == '/') {
                        pos_ += 2;
                        break;
                    }
                    if (src_[pos_] == '\n') {
                        ++line_;
                    }
                    ++pos_;
                }
                continue;
            }
            break;
        }
    }

    const std::string& src_;
    std::size_t pos_{0};
    int line_{1};
};

// ── Parser ────────────────────────────────────────────────────────────────────

// ParseFailure is thrown internally to unwind to the top-level entry point;
// never escapes parse_string()/parse_file().
struct ParseFailure {
    ParseError err;
};

class Parser {
public:
    explicit Parser(const std::string& src) : lx_(src) {}

    Module parse_module() {
        Module m;
        for (;;) {
            const Token& t = peek();
            if (t.kind == TokKind::eof || t.kind == TokKind::rbrace) {
                break;
            }
            if (t.kind != TokKind::ident) {
                consume();
                continue;
            }
            if (t.val == "module") {
                consume();
                auto sub = std::make_shared<Module>(parse_named_module());
                m.modules.push_back(std::move(sub));
            } else if (t.val == "struct") {
                consume();
                m.structs.push_back(parse_struct());
            } else if (t.val == "enum") {
                consume();
                m.enums.push_back(parse_enum());
            } else if (t.val == "typedef") {
                consume();
                m.typedefs.push_back(parse_typedef());
            } else {
                // Unknown keyword or annotation -- skip to next ';' or '}'.
                consume();
                for (;;) {
                    const Token& t2 = peek();
                    if (t2.kind == TokKind::eof || t2.kind == TokKind::semi || t2.kind == TokKind::rbrace) {
                        if (t2.kind == TokKind::semi) {
                            consume();
                        }
                        break;
                    }
                    consume();
                }
            }
        }
        return m;
    }

private:
    const Token& peek() {
        if (!peeked_) {
            cur_ = lx_.next();
            peeked_ = true;
        }
        return cur_;
    }

    Token consume() {
        Token t = peek();
        peeked_ = false;
        return t;
    }

    [[noreturn]] void fail(int line, const std::string& msg) {
        throw ParseFailure{ParseError{line, msg}};
    }

    Token expect_tok(TokKind kind, const std::string& desc) {
        Token t = consume();
        if (t.kind != kind) {
            fail(t.line, "idl: line " + std::to_string(t.line) + ": expected " + desc +
                             ", got \"" + t.val + "\"");
        }
        return t;
    }

    void expect(TokKind kind, const std::string& desc) { (void)expect_tok(kind, desc); }

    Module parse_named_module() {
        Token name_tok = expect_tok(TokKind::ident, "module name");
        expect(TokKind::lbrace, "{");
        Module m = parse_module();
        m.name = name_tok.val;
        expect(TokKind::rbrace, "}");
        if (peek().kind == TokKind::semi) {
            consume();
        }
        return m;
    }

    Struct parse_struct() {
        Token name_tok = expect_tok(TokKind::ident, "struct name");
        expect(TokKind::lbrace, "{");
        Struct s;
        s.name = name_tok.val;
        for (;;) {
            const Token& t = peek();
            if (t.kind == TokKind::rbrace || t.kind == TokKind::eof) {
                break;
            }
            bool is_key = false;
            while (peek().kind == TokKind::at) {
                consume(); // @
                Token annot = consume();
                if (annot.val == "key") {
                    is_key = true;
                }
                // Ignore annotation arguments: only a literal `{...}` block
                // (never actually produced by IDL's `(...)` syntax -- see
                // dds/idl/idl.hpp) would be skipped here.
                if (peek().kind == TokKind::lbrace) {
                    while (peek().kind != TokKind::rbrace && peek().kind != TokKind::eof) {
                        consume();
                    }
                    if (peek().kind == TokKind::rbrace) {
                        consume();
                    }
                }
            }
            TypeSpec type_spec = parse_type_spec();
            Token field_name = expect_tok(TokKind::ident, "field name");
            if (peek().kind == TokKind::lbracket) {
                consume(); // [
                Token size_tok = consume();
                if (size_tok.kind != TokKind::number) {
                    fail(size_tok.line, "idl: line " + std::to_string(size_tok.line) +
                                             ": expected array size, got \"" + size_tok.val + "\"");
                }
                expect(TokKind::rbracket, "]");
                int size = 0;
                try {
                    size = std::stoi(size_tok.val);
                } catch (const std::exception&) {
                    fail(size_tok.line, "idl: line " + std::to_string(size_tok.line) +
                                             ": invalid array size \"" + size_tok.val + "\"");
                }
                auto elem = std::make_shared<TypeSpec>(type_spec);
                type_spec = TypeSpec{};
                type_spec.kind = TypeKind::array;
                type_spec.elem_type = elem;
                type_spec.array_size = size;
            }
            expect(TokKind::semi, ";");
            Field f;
            f.name = field_name.val;
            f.type = type_spec;
            f.key = is_key;
            s.fields.push_back(std::move(f));
        }
        expect(TokKind::rbrace, "}");
        expect(TokKind::semi, ";");
        return s;
    }

    Enum parse_enum() {
        Token name_tok = expect_tok(TokKind::ident, "enum name");
        expect(TokKind::lbrace, "{");
        Enum e;
        e.name = name_tok.val;
        for (;;) {
            const Token& t = peek();
            if (t.kind == TokKind::rbrace || t.kind == TokKind::eof) {
                break;
            }
            Token cur = consume(); // always advance; prevents spin on unexpected tokens
            if (cur.kind == TokKind::ident) {
                e.values.push_back(cur.val);
            }
            if (peek().kind == TokKind::comma) {
                consume();
            }
        }
        expect(TokKind::rbrace, "}");
        expect(TokKind::semi, ";");
        return e;
    }

    Typedef parse_typedef() {
        TypeSpec underlying = parse_type_spec();
        Token name_tok = expect_tok(TokKind::ident, "typedef alias name");
        expect(TokKind::semi, ";");
        Typedef td;
        td.name = name_tok.val;
        td.type = underlying;
        return td;
    }

    TypeSpec parse_type_spec() {
        Token t = consume();
        if (t.kind != TokKind::ident) {
            fail(t.line, "idl: line " + std::to_string(t.line) + ": expected type name, got \"" +
                              t.val + "\"");
        }
        if (t.val == "boolean") return TypeSpec{TypeKind::boolean, nullptr, 0, ""};
        if (t.val == "octet") return TypeSpec{TypeKind::octet, nullptr, 0, ""};
        if (t.val == "float") return TypeSpec{TypeKind::float_, nullptr, 0, ""};
        if (t.val == "double") return TypeSpec{TypeKind::double_, nullptr, 0, ""};
        if (t.val == "string") {
            if (peek().kind == TokKind::langle) {
                consume(); // <
                consume(); // bound value -- ignored
                expect(TokKind::rangle, ">");
            }
            return TypeSpec{TypeKind::string, nullptr, 0, ""};
        }
        if (t.val == "short") return TypeSpec{TypeKind::short_, nullptr, 0, ""};
        if (t.val == "long") {
            if (peek().val == "long") {
                consume();
                return TypeSpec{TypeKind::long_long, nullptr, 0, ""};
            }
            return TypeSpec{TypeKind::long_, nullptr, 0, ""};
        }
        if (t.val == "unsigned") {
            Token next = consume();
            if (next.kind != TokKind::ident) {
                fail(next.line, "idl: line " + std::to_string(next.line) +
                                     ": expected type after 'unsigned', got \"" + next.val + "\"");
            }
            if (next.val == "short") return TypeSpec{TypeKind::ushort, nullptr, 0, ""};
            if (next.val == "long") {
                if (peek().val == "long") {
                    consume();
                    return TypeSpec{TypeKind::ulong_long, nullptr, 0, ""};
                }
                return TypeSpec{TypeKind::ulong, nullptr, 0, ""};
            }
            fail(next.line, "idl: line " + std::to_string(next.line) + ": unknown unsigned type \"" +
                                 next.val + "\"");
        }
        if (t.val == "sequence") {
            expect(TokKind::langle, "<");
            auto elem = std::make_shared<TypeSpec>(parse_type_spec());
            if (peek().kind == TokKind::comma) {
                consume();
                consume(); // bound value -- ignored
            }
            expect(TokKind::rangle, ">");
            TypeSpec ts;
            ts.kind = TypeKind::sequence;
            ts.elem_type = elem;
            return ts;
        }
        // Named type -- may be qualified: Module::Struct or bare Struct/Enum
        // name. Consume any leading '::' segments to form the full
        // qualified name. Struct-vs-enum resolution is deferred to the
        // code generator (both use ref_name), matching go-DDS exactly.
        std::string name = t.val;
        while (peek().kind == TokKind::double_colon) {
            consume(); // ::
            Token next = consume();
            if (next.kind != TokKind::ident) {
                fail(next.line, "idl: line " + std::to_string(next.line) +
                                     ": expected name after '::', got \"" + next.val + "\"");
            }
            name += "::" + next.val;
        }
        TypeSpec ts;
        ts.kind = TypeKind::struct_;
        ts.ref_name = name;
        return ts;
    }

    Lexer lx_;
    Token cur_;
    bool peeked_{false};
};

} // namespace

ParseResult parse_string(const std::string& src) {
    ParseResult result;
    Parser p(src);
    try {
        result.module = p.parse_module();
    } catch (const ParseFailure& f) {
        result.error = f.err;
    }
    return result;
}

ParseResult parse_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ParseResult result;
        result.error = ParseError{0, "idl: read " + path + ": could not open file"};
        return result;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_string(ss.str());
}

} // namespace dds::idl
