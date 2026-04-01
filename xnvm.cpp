// xnvm.cpp -- XN bytecode compiler + stack VM  v3
// Compile (MSVC): cl /O2 /std:c++20 /EHsc /utf-8 xnvm.cpp /Fe:xnvm.exe
// Compile (GCC) : g++ -O2 -std=c++20 xnvm.cpp -o xnvm
// Usage: xnvm <file.xn>   |   xnvm --build <file.xn> [out.exe]

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <variant>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cassert>
#include <numeric>
#include <functional>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

// ══════════════════════════════════════════════════════════════════════════════
// BYTECODE
// ══════════════════════════════════════════════════════════════════════════════

enum class Op : uint8_t {
    PUSH_CONST, PUSH_NIL, PUSH_TRUE,
    LOAD, STORE,
    MAKE_CLOSURE, CALL, RETURN,
    JUMP, JUMP_IF_FALSE, POP,
    // node
    IS_NIL, GET_FIELD, MAKE_NODE, MATCH_NODE,
    // list
    MAKE_LIST, CONCAT,
    LIST_LEN, LIST_SUM, LIST_SORT, LIST_IDX,
    LIST_FOLD, LIST_MAP, LIST_FILTER,
    LIST_ANY, LIST_ALL, LIST_FIND, LIST_FLAT,
    RANGE,
    // arithmetic
    ADD, SUB, MUL, DIV, MOD, POW,
    LT, GT, EQ, LE, GE, NEG, NOT,
    // math
    SQRT,
    // io
    PRINT, SEQ, INPUT,
    // string
    STR_CAT, STR_LEN, STR_GET, STR_SUB,
    STR_SPLIT, STR_TRIM, STR_FIND,
    STR_NUM, NUM_STR, STR_UPPER, STR_LOWER,
    STR_STARTS, STR_ENDS, STR_REPLACE,
    // dict
    MAKE_DICT, DICT_SET, DICT_GET, DICT_HAS,
    DICT_DEL, DICT_KEYS, DICT_VALS, DICT_LEN, DICT_MERGE,
    // misc
    TYPE_OF, APPLY, TRY, EVAL,
};

struct Instr { Op op; int64_t arg = 0; };

// ══════════════════════════════════════════════════════════════════════════════
// VALUES
// ══════════════════════════════════════════════════════════════════════════════

struct Value;
struct Env;
struct Closure;
struct CodeObj;

using Val  = std::shared_ptr<Value>;
using EnvP = std::shared_ptr<Env>;
using Code = std::shared_ptr<CodeObj>;
using Dict = std::shared_ptr<std::unordered_map<std::string, Val>>;

struct XNode   { Val left, right, val; };
struct Closure { Code code; EnvP env; };

struct CodeObj {
    std::string              name;
    int                      arity    = 0;
    bool                     variadic = false; // (λ (x . rest) body)
    std::vector<Instr>       code;
    std::vector<std::string> names;
    std::vector<Val>         consts;

    int nameIdx(const std::string& s) {
        for (int i = 0; i < (int)names.size(); i++)
            if (names[i] == s) return i;
        names.push_back(s); return (int)names.size()-1;
    }
    int addConst(Val v) { consts.push_back(v); return (int)consts.size()-1; }
};

struct Value {
    using Data = std::variant<
        std::monostate,           // NIL
        bool,                     // BOOL
        int64_t,                  // INT
        double,                   // FLOAT
        std::string,              // STR
        std::vector<Val>,         // LIST
        std::shared_ptr<XNode>,   // NODE
        std::shared_ptr<Closure>, // CLOSURE
        Dict                      // DICT
    >;
    Data data;

    bool isNil()     const { return std::holds_alternative<std::monostate>(data); }
    bool isBool()    const { return std::holds_alternative<bool>(data); }
    bool isInt()     const { return std::holds_alternative<int64_t>(data); }
    bool isDbl()     const { return std::holds_alternative<double>(data); }
    bool isStr()     const { return std::holds_alternative<std::string>(data); }
    bool isList()    const { return std::holds_alternative<std::vector<Val>>(data); }
    bool isNode()    const { return std::holds_alternative<std::shared_ptr<XNode>>(data); }
    bool isClosure() const { return std::holds_alternative<std::shared_ptr<Closure>>(data); }
    bool isDict()    const { return std::holds_alternative<Dict>(data); }

    int64_t    asInt()  const { return std::get<int64_t>(data); }
    bool       asBool() const { return std::get<bool>(data); }
    double     asDbl()  const {
        if (isInt()) return (double)std::get<int64_t>(data);
        return std::get<double>(data);
    }
    const std::string&       asStr()     const { return std::get<std::string>(data); }
    const std::vector<Val>&  asList()    const { return std::get<std::vector<Val>>(data); }
    std::vector<Val>&        asList()          { return std::get<std::vector<Val>>(data); }
    std::shared_ptr<XNode>   asNode()    const { return std::get<std::shared_ptr<XNode>>(data); }
    std::shared_ptr<Closure> asClosure() const { return std::get<std::shared_ptr<Closure>>(data); }
    Dict                     asDict()    const { return std::get<Dict>(data); }

    bool truthy() const {
        if (isNil())  return false;
        if (isBool()) return asBool();
        return true;
    }

    std::string repr() const {
        if (isNil())    return "None";
        if (isBool())   return asBool() ? "True" : "False";
        if (isInt())    return std::to_string(asInt());
        if (isDbl())    { std::ostringstream os; os << asDbl(); return os.str(); }
        if (isStr())    return asStr();
        if (isList()) {
            std::string s = "[";
            for (int i=0; i<(int)asList().size(); i++) {
                if (i) s += ", ";
                s += asList()[i]->repr();
            }
            return s + "]";
        }
        if (isNode())    return "Node(" + asNode()->val->repr() + ")";
        if (isClosure()) return "<fn:" + asClosure()->code->name + ">";
        if (isDict()) {
            std::string s = "{";
            bool first = true;
            for (auto& [k,v] : *asDict()) {
                if (!first) s += ", ";
                s += "\"" + k + "\": " + v->repr();
                first = false;
            }
            return s + "}";
        }
        return "?";
    }
};

static Val NIL_V  = std::make_shared<Value>(Value{std::monostate{}});
static Val TRUE_V = std::make_shared<Value>(Value{true});

inline Val mkInt (int64_t v)            { return std::make_shared<Value>(Value{v}); }
inline Val mkDbl (double  v)            { return std::make_shared<Value>(Value{v}); }
inline Val mkStr (std::string v)        { return std::make_shared<Value>(Value{std::move(v)}); }
inline Val mkList(std::vector<Val> v)   { return std::make_shared<Value>(Value{std::move(v)}); }
inline Val mkNode(Val l, Val r, Val v) {
    return std::make_shared<Value>(Value{std::make_shared<XNode>(XNode{l,r,v})});
}
inline Val mkClosure(Code c, EnvP e) {
    return std::make_shared<Value>(Value{std::make_shared<Closure>(Closure{c,e})});
}
inline Val mkDict(std::unordered_map<std::string,Val> m = {}) {
    return std::make_shared<Value>(Value{
        std::make_shared<std::unordered_map<std::string,Val>>(std::move(m))
    });
}

// ══════════════════════════════════════════════════════════════════════════════
// ENVIRONMENT
// ══════════════════════════════════════════════════════════════════════════════

struct Env {
    std::unordered_map<std::string, Val> vars;
    EnvP parent;
    explicit Env(EnvP p = nullptr) : parent(p) {}

    Val get(const std::string& n) const {
        auto it = vars.find(n);
        if (it != vars.end()) return it->second;
        if (parent) return parent->get(n);
        throw std::runtime_error("Undefined: " + n);
    }
    void set(const std::string& n, Val v) { vars[n] = v; }
};

// ══════════════════════════════════════════════════════════════════════════════
// LEXER
// ══════════════════════════════════════════════════════════════════════════════

enum class TK { LP,RP,LB,RB,LSQ,RSQ, INT,FLT,STR,ATOM, END };
struct Token { TK type; std::string val; int64_t ival=0; double fval=0.0; };

struct Lexer {
    const std::string& src;
    size_t pos = 0;
    explicit Lexer(const std::string& s) : src(s) {}

    void skip() {
        while (pos < src.size()) {
            if (src[pos]=='-' && pos+1<src.size() && src[pos+1]=='-')
                while (pos<src.size() && src[pos]!='\n') pos++;
            else if ((unsigned char)src[pos]<=32) pos++;
            else break;
        }
    }

    static bool isAtomByte(char c) {
        if ((unsigned char)c<=32) return false;
        return c!='('&&c!=')'&&c!='{'&&c!='}'&&c!='['&&c!=']'&&c!='"';
    }

    Token next() {
        skip();
        if (pos>=src.size()) return {TK::END};
        char c = src[pos];
        if (c=='(') { pos++; return {TK::LP}; }
        if (c==')') { pos++; return {TK::RP}; }
        if (c=='{') { pos++; return {TK::LB}; }
        if (c=='}') { pos++; return {TK::RB}; }
        if (c=='[') { pos++; return {TK::LSQ}; }
        if (c==']') { pos++; return {TK::RSQ}; }
        if (c=='"') {
            pos++;
            std::string s;
            while (pos<src.size() && src[pos]!='"') {
                if (src[pos]=='\\') {
                    pos++;
                    char e = src[pos++];
                    switch(e) {
                        case 'n': s+='\n'; break;
                        case 't': s+='\t'; break;
                        case 'r': s+='\r'; break;
                        default:  s+=e;    break;
                    }
                } else s+=src[pos++];
            }
            pos++;
            return {TK::STR, s};
        }
        size_t start = pos;
        while (pos<src.size() && isAtomByte(src[pos])) {
            unsigned char u=(unsigned char)src[pos];
            if      (u<0x80)         pos+=1;
            else if ((u&0xE0)==0xC0) pos+=2;
            else if ((u&0xF0)==0xE0) pos+=3;
            else                     pos+=4;
        }
        std::string atom = src.substr(start, pos-start);
        try { size_t i; int64_t v=std::stoll(atom,&i); if(i==atom.size()) return {TK::INT,atom,v}; } catch(...) {}
        try { size_t i; double  v=std::stod (atom,&i); if(i==atom.size()) return {TK::FLT,atom,0,v}; } catch(...) {}
        return {TK::ATOM, atom};
    }

    std::vector<Token> all() {
        std::vector<Token> t;
        for(;;) { auto tok=next(); t.push_back(tok); if(tok.type==TK::END) break; }
        return t;
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// AST
// ══════════════════════════════════════════════════════════════════════════════

struct ANode;
using AST = std::shared_ptr<ANode>;

struct ANode {
    enum class K { ATOM,INT,FLT,STR,LIST,STRUCT,SEXPR } kind;
    std::string      sval;
    int64_t          ival = 0;
    double           fval = 0.0;
    std::vector<AST> ch;
};

static AST makeANode(ANode::K k) { auto n=std::make_shared<ANode>(); n->kind=k; return n; }

static AST atomNode(const std::string& s) {
    auto n = makeANode(ANode::K::ATOM); n->sval = s; return n;
}

struct Parser {
    const std::vector<Token>& toks;
    size_t pos=0;
    explicit Parser(const std::vector<Token>& t): toks(t) {}

    const Token& peek() const { return toks[pos]; }
    const Token& eat()        { return toks[pos++]; }

    AST expr() {
        auto& t = peek();
        switch (t.type) {
        case TK::INT:  { auto n=makeANode(ANode::K::INT);    n->ival=t.ival; eat(); return n; }
        case TK::FLT:  { auto n=makeANode(ANode::K::FLT);    n->fval=t.fval; eat(); return n; }
        case TK::STR:  { auto n=makeANode(ANode::K::STR);    n->sval=t.val;  eat(); return n; }
        case TK::ATOM: { auto n=makeANode(ANode::K::ATOM);   n->sval=t.val;  eat(); return n; }
        case TK::LP: {
            eat();
            auto n=makeANode(ANode::K::SEXPR);
            while(peek().type!=TK::RP) {
                if(peek().type==TK::END) throw std::runtime_error("Unclosed (");
                n->ch.push_back(expr());
            }
            eat(); return n;
        }
        case TK::LB: {
            eat();
            auto n=makeANode(ANode::K::STRUCT);
            while(peek().type!=TK::RB) {
                if(peek().type==TK::END) throw std::runtime_error("Unclosed {");
                n->ch.push_back(expr());
            }
            eat();
            if(n->ch.size()!=3) throw std::runtime_error("Struct must have exactly 3 fields");
            return n;
        }
        case TK::LSQ: {
            eat();
            auto n=makeANode(ANode::K::LIST);
            while(peek().type!=TK::RSQ) {
                if(peek().type==TK::END) throw std::runtime_error("Unclosed [");
                n->ch.push_back(expr());
            }
            eat(); return n;
        }
        default: throw std::runtime_error("Unexpected token: '" + t.val + "'");
        }
    }

    std::vector<AST> program() {
        std::vector<AST> r;
        while(peek().type!=TK::END) r.push_back(expr());
        return r;
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// COMPILER
// ══════════════════════════════════════════════════════════════════════════════

struct Compiler {
    std::vector<Code> pool;
    Code              co;

    Compiler() { co=std::make_shared<CodeObj>(); co->name="<module>"; }

    void emit(Op op, int64_t arg=0) { co->code.push_back({op,arg}); }
    int  addConst(Val v)            { return co->addConst(v); }
    int  ip()                       { return (int)co->code.size(); }
    void patch(int idx)             { co->code[idx].arg=ip(); }

    static std::string atomOf(AST n) {
        return (n->kind==ANode::K::ATOM) ? n->sval : "";
    }

    void compExpr(AST n) {
        using K=ANode::K;
        switch(n->kind) {
        case K::INT:    emit(Op::PUSH_CONST, addConst(mkInt(n->ival))); return;
        case K::FLT:    emit(Op::PUSH_CONST, addConst(mkDbl(n->fval))); return;
        case K::STR:    emit(Op::PUSH_CONST, addConst(mkStr(n->sval))); return;
        case K::ATOM: {
            auto& s=n->sval;
            if(s=="\xe2\x8a\xa5") { emit(Op::PUSH_NIL);  return; } // ⊥
            if(s=="\xe2\x8a\xa4") { emit(Op::PUSH_TRUE); return; } // ⊤
            emit(Op::LOAD, co->nameIdx(s));
            return;
        }
        case K::LIST:
            for(auto& c:n->ch) compExpr(c);
            emit(Op::MAKE_LIST, (int)n->ch.size());
            return;
        case K::STRUCT:
            compExpr(n->ch[0]); compExpr(n->ch[1]); compExpr(n->ch[2]);
            emit(Op::MAKE_NODE);
            return;
        case K::SEXPR:
            compSexpr(n);
            return;
        }
    }

    void compSexpr(AST n) {
        auto& ch=n->ch;
        if(ch.empty()) { emit(Op::PUSH_NIL); return; }
        std::string h=atomOf(ch[0]);

        // ── lambda ────────────────────────────────────────────────────────────
        if(h=="\xce\xbb"||h=="\\") { compLambda(ch[1], ch[2]); return; }

        // ── pipe ──────────────────────────────────────────────────────────────
        if(h=="->") {
            AST cur=ch[1];
            for(int i=2;i<(int)ch.size();i++) {
                AST step=ch[i];
                auto call=makeANode(ANode::K::SEXPR);
                if(step->kind==ANode::K::ATOM) {
                    call->ch.push_back(step); call->ch.push_back(cur);
                } else {
                    call->ch.push_back(step->ch[0]); call->ch.push_back(cur);
                    for(int j=1;j<(int)step->ch.size();j++) call->ch.push_back(step->ch[j]);
                }
                cur=call;
            }
            compExpr(cur); return;
        }

        // ── let ───────────────────────────────────────────────────────────────
        if(h=="let") {
            auto& binds=ch[1]->ch;
            for(int i=0;i+1<(int)binds.size();i+=2) {
                compExpr(binds[i+1]);
                emit(Op::STORE, co->nameIdx(binds[i]->sval));
            }
            compExpr(ch[2]); return;
        }

        // ── match ─────────────────────────────────────────────────────────────
        if(h=="match") {
            compExpr(ch[1]); emit(Op::MATCH_NODE);
            auto& pat=ch[2]->ch;
            emit(Op::STORE, co->nameIdx(pat[2]->sval));
            emit(Op::STORE, co->nameIdx(pat[1]->sval));
            emit(Op::STORE, co->nameIdx(pat[0]->sval));
            compExpr(ch[3]); return;
        }

        // ── if ────────────────────────────────────────────────────────────────
        if(h=="if") {
            compExpr(ch[1]);
            int jf=ip(); emit(Op::JUMP_IF_FALSE,0);
            compExpr(ch[2]);
            int je=ip(); emit(Op::JUMP,0);
            patch(jf); compExpr(ch[3]); patch(je); return;
        }

        // ── : (single let) ────────────────────────────────────────────────────
        if(h==":") {
            compExpr(ch[2]);
            emit(Op::STORE, co->nameIdx(ch[1]->sval));
            compExpr(ch[3]); return;
        }

        // ── try ───────────────────────────────────────────────────────────────
        // (try expr handler)  or  (try expr)
        // Wraps expr in a thunk so we can catch C++ exceptions from it.
        if(h=="try") {
            // Build: (λ () expr)
            auto thunk = makeANode(ANode::K::SEXPR);
            thunk->ch.push_back(atomNode("\xce\xbb"));
            thunk->ch.push_back(makeANode(ANode::K::SEXPR)); // empty params
            thunk->ch.push_back(ch[1]);
            compExpr(thunk);

            if(ch.size()>=3) {
                compExpr(ch[2]);  // user handler
            } else {
                // default handler: (λ (e) ⊥)
                auto defh = makeANode(ANode::K::SEXPR);
                defh->ch.push_back(atomNode("\xce\xbb"));
                auto ep = makeANode(ANode::K::SEXPR);
                ep->ch.push_back(atomNode("__e"));
                defh->ch.push_back(ep);
                defh->ch.push_back(atomNode("\xe2\x8a\xa5")); // ⊥
                compExpr(defh);
            }
            emit(Op::TRY); return;
        }

        // ── predicates & builtins ─────────────────────────────────────────────
        if(h=="\xe2\x8a\xa5?") { compExpr(ch[1]); emit(Op::IS_NIL); return; }
        if(!h.empty()&&h[0]=='.') {
            std::string f=h.substr(1);
            int fi=(f=="left")?0:(f=="right")?1:2;
            compExpr(ch[1]); emit(Op::GET_FIELD,fi); return;
        }

        if(h=="++")      { for(int i=1;i<(int)ch.size();i++) compExpr(ch[i]); emit(Op::CONCAT,(int)ch.size()-1); return; }
        if(h=="io")      { for(int i=1;i<(int)ch.size();i++) compExpr(ch[i]); emit(Op::SEQ,(int)ch.size()-1); return; }

        if(h=="fold")    { compExpr(ch[1]);compExpr(ch[2]);compExpr(ch[3]); emit(Op::LIST_FOLD);   return; }
        if(h=="map")     { compExpr(ch[1]);compExpr(ch[2]); emit(Op::LIST_MAP);    return; }
        if(h=="filter")  { compExpr(ch[1]);compExpr(ch[2]); emit(Op::LIST_FILTER); return; }
        if(h=="sort")    { compExpr(ch[1]); emit(Op::LIST_SORT); return; }
        if(h=="@")       { compExpr(ch[1]);compExpr(ch[2]); emit(Op::LIST_IDX);    return; }
        if(h=="any?")    { compExpr(ch[1]);compExpr(ch[2]); emit(Op::LIST_ANY);    return; }
        if(h=="all?")    { compExpr(ch[1]);compExpr(ch[2]); emit(Op::LIST_ALL);    return; }
        if(h=="find")    { compExpr(ch[1]);compExpr(ch[2]); emit(Op::LIST_FIND);   return; }
        if(h=="flat")    { compExpr(ch[1]); emit(Op::LIST_FLAT); return; }

        if(h=="range")   { for(int i=1;i<(int)ch.size();i++) compExpr(ch[i]); emit(Op::RANGE,(int)ch.size()-1); return; }
        if(h=="print")   { for(int i=1;i<(int)ch.size();i++) compExpr(ch[i]); emit(Op::PRINT,(int)ch.size()-1); return; }

        if(h=="\xce\xa3")           { compExpr(ch[1]); emit(Op::LIST_SUM); return; } // Σ
        if(h=="#")                   { compExpr(ch[1]); emit(Op::LIST_LEN); return; }
        if(h=="\xe2\x88\x9a")       { compExpr(ch[1]); emit(Op::SQRT);     return; } // √
        if(h=="not")                 { compExpr(ch[1]); emit(Op::NOT);      return; }
        if(h=="input")               { emit(Op::INPUT);                     return; }
        if(h=="type")                { compExpr(ch[1]); emit(Op::TYPE_OF);  return; }
        if(h=="apply")               { compExpr(ch[1]);compExpr(ch[2]); emit(Op::APPLY); return; }
        if(h=="eval")                { compExpr(ch[1]); emit(Op::EVAL);     return; }

        // string ops
        if(h=="str-cat")    { for(int i=1;i<(int)ch.size();i++) compExpr(ch[i]); emit(Op::STR_CAT,(int)ch.size()-1); return; }
        if(h=="str-len")    { compExpr(ch[1]); emit(Op::STR_LEN);  return; }
        if(h=="str-get")    { compExpr(ch[1]);compExpr(ch[2]); emit(Op::STR_GET); return; }
        if(h=="str-sub")    { compExpr(ch[1]);compExpr(ch[2]);compExpr(ch[3]); emit(Op::STR_SUB);     return; }
        if(h=="str-split")  { compExpr(ch[1]);compExpr(ch[2]); emit(Op::STR_SPLIT);  return; }
        if(h=="str-trim")   { compExpr(ch[1]); emit(Op::STR_TRIM);   return; }
        if(h=="str-find")   { compExpr(ch[1]);compExpr(ch[2]); emit(Op::STR_FIND);   return; }
        if(h=="str->num")   { compExpr(ch[1]); emit(Op::STR_NUM);    return; }
        if(h=="num->str")   { compExpr(ch[1]); emit(Op::NUM_STR);    return; }
        if(h=="str-upper")  { compExpr(ch[1]); emit(Op::STR_UPPER);  return; }
        if(h=="str-lower")  { compExpr(ch[1]); emit(Op::STR_LOWER);  return; }
        if(h=="str-starts?") { compExpr(ch[1]);compExpr(ch[2]); emit(Op::STR_STARTS); return; }
        if(h=="str-ends?")   { compExpr(ch[1]);compExpr(ch[2]); emit(Op::STR_ENDS);   return; }
        if(h=="str-replace") { compExpr(ch[1]);compExpr(ch[2]);compExpr(ch[3]); emit(Op::STR_REPLACE); return; }

        // dict ops
        if(h=="dict")       { emit(Op::MAKE_DICT);  return; }
        if(h=="dict-set")   { compExpr(ch[1]);compExpr(ch[2]);compExpr(ch[3]); emit(Op::DICT_SET);   return; }
        if(h=="dict-get")   { compExpr(ch[1]);compExpr(ch[2]); emit(Op::DICT_GET);   return; }
        if(h=="dict-has?")  { compExpr(ch[1]);compExpr(ch[2]); emit(Op::DICT_HAS);   return; }
        if(h=="dict-del")   { compExpr(ch[1]);compExpr(ch[2]); emit(Op::DICT_DEL);   return; }
        if(h=="dict-keys")  { compExpr(ch[1]); emit(Op::DICT_KEYS);  return; }
        if(h=="dict-vals")  { compExpr(ch[1]); emit(Op::DICT_VALS);  return; }
        if(h=="dict-len")   { compExpr(ch[1]); emit(Op::DICT_LEN);   return; }
        if(h=="dict-merge") { compExpr(ch[1]);compExpr(ch[2]); emit(Op::DICT_MERGE); return; }

        // arithmetic / comparison
        static const std::pair<const char*,Op> BINOPS[] = {
            {"+",Op::ADD},{"-",Op::SUB},{"*",Op::MUL},{"/",Op::DIV},
            {"%",Op::MOD},{"^",Op::POW},
            {"<",Op::LT},{">",Op::GT},{"=",Op::EQ},{"<=",Op::LE},{">=",Op::GE}
        };
        for(auto& [sym,op]:BINOPS)
            if(h==sym) { compExpr(ch[1]); compExpr(ch[2]); emit(op); return; }

        if(h=="neg") { compExpr(ch[1]); emit(Op::NEG); return; }

        // general call
        compExpr(ch[0]);
        for(int i=1;i<(int)ch.size();i++) compExpr(ch[i]);
        emit(Op::CALL,(int)ch.size()-1);
    }

    void compLambda(AST params, AST body, const std::string& name="<lambda>") {
        auto saved=co;
        co=std::make_shared<CodeObj>();
        co->name=name;
        if(params->kind==ANode::K::SEXPR) {
            for(int i=0;i<(int)params->ch.size();i++) {
                if(params->ch[i]->sval==".") {
                    co->variadic=true;
                    if(i+1<(int)params->ch.size()) co->nameIdx(params->ch[i+1]->sval);
                    break;
                }
                co->nameIdx(params->ch[i]->sval);
                co->arity++;
            }
        }
        compExpr(body);
        emit(Op::RETURN);
        int idx=(int)pool.size(); pool.push_back(co);
        co=saved;
        emit(Op::MAKE_CLOSURE,idx);
    }

    void compDecl(AST n) {
        auto& ch=n->ch;
        std::string name=ch[1]->sval;
        AST expr=ch[2];
        bool isLam=(expr->kind==ANode::K::SEXPR && !expr->ch.empty()
                    && (expr->ch[0]->sval=="\xce\xbb"||expr->ch[0]->sval=="\\"));
        if(isLam) compLambda(expr->ch[1], expr->ch[2], name);
        else      compExpr(expr);
        emit(Op::STORE, co->nameIdx(name));
    }

    // returnLast=true: last expression value is kept on stack (used by eval)
    void compProgram(const std::vector<AST>& prog, bool returnLast=false) {
        bool hasMain=false;
        for(int ni=0;ni<(int)prog.size();ni++) {
            auto& n=prog[ni];
            bool isDecl=(n->kind==ANode::K::SEXPR && !n->ch.empty() && n->ch[0]->sval=="=");
            bool isLast=(ni==(int)prog.size()-1);
            if(isDecl) {
                if(n->ch[1]->sval=="main") hasMain=true;
                compDecl(n);
            } else {
                compExpr(n);
                if(!(returnLast && isLast && !hasMain)) emit(Op::POP);
            }
        }
        if(hasMain) { emit(Op::LOAD,co->nameIdx("main")); emit(Op::CALL,0); emit(Op::POP); }
        emit(Op::RETURN);
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// VM
// ══════════════════════════════════════════════════════════════════════════════

struct VM {
    Compiler& C;
    EnvP      globals;

    explicit VM(Compiler& c) : C(c), globals(std::make_shared<Env>()) {}

    Val callClosure(const std::shared_ptr<Closure>& clo, std::vector<Val> args) {
        auto& co=clo->code;
        if(!co->variadic && (int)args.size()!=co->arity)
            throw std::runtime_error(co->name+": expected "+std::to_string(co->arity)
                +" args, got "+std::to_string(args.size()));
        if(co->variadic && (int)args.size()<co->arity)
            throw std::runtime_error(co->name+": expected at least "+std::to_string(co->arity)
                +" args, got "+std::to_string(args.size()));
        auto frame=std::make_shared<Env>(clo->env);
        for(int i=0;i<co->arity;i++) frame->set(co->names[i], args[i]);
        if(co->variadic && co->arity < (int)co->names.size()) {
            std::vector<Val> rest(args.begin()+co->arity, args.end());
            frame->set(co->names[co->arity], mkList(std::move(rest)));
        }
        return exec(co, frame);
    }

    Val exec(Code startCo, EnvP startEnv) {
        Code co  = startCo;
        EnvP env = startEnv;

    tco_restart:
        std::vector<Val> stack;
        stack.reserve(64);
        int ip=0;

        auto push = [&](Val v)      { stack.push_back(std::move(v)); };
        auto pop  = [&]() -> Val    { Val v=stack.back(); stack.pop_back(); return v; };

        while(ip<(int)co->code.size()) {
            auto& ins=co->code[ip++];
            switch(ins.op) {

            case Op::PUSH_CONST: push(co->consts[ins.arg]); break;
            case Op::PUSH_NIL:   push(NIL_V);  break;
            case Op::PUSH_TRUE:  push(TRUE_V); break;
            case Op::LOAD:       push(env->get(co->names[ins.arg])); break;
            case Op::STORE:      env->set(co->names[ins.arg], pop()); break;

            case Op::MAKE_CLOSURE:
                push(mkClosure(C.pool[ins.arg], env)); break;

            case Op::CALL: {
                int n=(int)ins.arg;
                std::vector<Val> args(n);
                for(int i=n-1;i>=0;i--) args[i]=pop();
                Val fn=pop();
                if(!fn->isClosure()) throw std::runtime_error("Not callable: "+fn->repr());
                auto clo=fn->asClosure();
                // TCO: if immediately followed by RETURN, reuse frame
                bool tail=(ip<(int)co->code.size() && co->code[ip].op==Op::RETURN);
                if(tail) {
                    if(!clo->code->variadic && (int)args.size()!=clo->code->arity)
                        throw std::runtime_error(clo->code->name+": arity mismatch in TCO");
                    auto frame=std::make_shared<Env>(clo->env);
                    for(int i=0;i<clo->code->arity;i++) frame->set(clo->code->names[i], args[i]);
                    if(clo->code->variadic && clo->code->arity<(int)clo->code->names.size()) {
                        std::vector<Val> rest(args.begin()+clo->code->arity, args.end());
                        frame->set(clo->code->names[clo->code->arity], mkList(std::move(rest)));
                    }
                    co=clo->code; env=frame;
                    goto tco_restart;
                }
                push(callClosure(clo, std::move(args)));
                break;
            }

            case Op::RETURN:
                return stack.empty() ? NIL_V : pop();

            case Op::JUMP:          ip=(int)ins.arg; break;
            case Op::JUMP_IF_FALSE: { Val c=pop(); if(!c->truthy()) ip=(int)ins.arg; break; }
            case Op::POP:           pop(); break;

            case Op::IS_NIL: { Val v=pop(); push(v->isNil()?TRUE_V:NIL_V); break; }
            case Op::NOT:    { Val v=pop(); push(v->truthy()?NIL_V:TRUE_V); break; }

            case Op::GET_FIELD: {
                Val v=pop();
                if(!v->isNode()) throw std::runtime_error("Field access on non-node: "+v->repr());
                auto nd=v->asNode();
                switch(ins.arg) { case 0: push(nd->left); break; case 1: push(nd->right); break; default: push(nd->val); }
                break;
            }
            case Op::MAKE_NODE: { Val v=pop(),r=pop(),l=pop(); push(mkNode(l,r,v)); break; }

            case Op::MAKE_LIST: {
                int n=(int)ins.arg; std::vector<Val> items(n);
                for(int i=n-1;i>=0;i--) items[i]=pop();
                push(mkList(std::move(items))); break;
            }
            case Op::CONCAT: {
                int n=(int)ins.arg; std::vector<Val> lists(n);
                for(int i=n-1;i>=0;i--) lists[i]=pop();
                std::vector<Val> res;
                for(auto& l:lists)
                    if(l->isList()) for(auto& x:l->asList()) res.push_back(x);
                    else res.push_back(l);
                push(mkList(std::move(res))); break;
            }

            #define BCAST_OP(OP) \
                { Val b_=pop(),a_=pop(); \
                  auto F=[](Val a,Val b)->Val{ return OP; }; \
                  if(!a_->isList()&&!b_->isList()){push(F(a_,b_));break;} \
                  if(a_->isList()&&b_->isList()){auto&la=a_->asList();auto&lb=b_->asList();std::vector<Val>r;r.reserve(la.size());for(int _i=0;_i<(int)la.size();_i++)r.push_back(F(la[_i],lb[_i]));push(mkList(std::move(r)));} \
                  else if(a_->isList()){std::vector<Val>r;for(auto&x:a_->asList())r.push_back(F(x,b_));push(mkList(std::move(r)));} \
                  else{std::vector<Val>r;for(auto&y:b_->asList())r.push_back(F(a_,y));push(mkList(std::move(r)));} \
                  break; }

            case Op::ADD: {
                Val b=pop(),a=pop();
                if(a->isStr()||b->isStr()){push(mkStr(a->repr()+b->repr()));break;}
                if(a->isInt()&&b->isInt()){push(mkInt(a->asInt()+b->asInt()));break;}
                if((a->isInt()||a->isDbl())&&(b->isInt()||b->isDbl())){push(mkDbl(a->asDbl()+b->asDbl()));break;}
                // broadcast
                auto F=[](Val a,Val b)->Val{
                    if(a->isStr()||b->isStr()) return mkStr(a->repr()+b->repr());
                    return a->isInt()&&b->isInt()?mkInt(a->asInt()+b->asInt()):mkDbl(a->asDbl()+b->asDbl());
                };
                if(a->isList()&&b->isList()){auto&la=a->asList();auto&lb=b->asList();std::vector<Val>r;r.reserve(la.size());for(int i=0;i<(int)la.size();i++)r.push_back(F(la[i],lb[i]));push(mkList(std::move(r)));}
                else if(a->isList()){std::vector<Val>r;for(auto&x:a->asList())r.push_back(F(x,b));push(mkList(std::move(r)));}
                else{std::vector<Val>r;for(auto&y:b->asList())r.push_back(F(a,y));push(mkList(std::move(r)));}
                break;
            }
            case Op::SUB: BCAST_OP(a->isInt()&&b->isInt()?mkInt(a->asInt()-b->asInt()):mkDbl(a->asDbl()-b->asDbl()))
            case Op::MUL: BCAST_OP(a->isInt()&&b->isInt()?mkInt(a->asInt()*b->asInt()):mkDbl(a->asDbl()*b->asDbl()))
            case Op::DIV: BCAST_OP((b->asDbl()==0.0?(throw std::runtime_error("division by zero"),mkDbl(0)):mkDbl(a->asDbl()/b->asDbl())))
            case Op::MOD: BCAST_OP(mkInt(a->asInt()%b->asInt()))
            case Op::POW: BCAST_OP(mkDbl(std::pow(a->asDbl(),b->asDbl())))

            case Op::LT: { Val b=pop(),a=pop(); push(a->asDbl()<b->asDbl() ?TRUE_V:NIL_V); break; }
            case Op::GT: { Val b=pop(),a=pop(); push(a->asDbl()>b->asDbl() ?TRUE_V:NIL_V); break; }
            case Op::LE: { Val b=pop(),a=pop(); push(a->asDbl()<=b->asDbl()?TRUE_V:NIL_V); break; }
            case Op::GE: { Val b=pop(),a=pop(); push(a->asDbl()>=b->asDbl()?TRUE_V:NIL_V); break; }
            case Op::EQ: {
                Val b=pop(),a=pop(); bool eq=false;
                if(a->isNil()&&b->isNil()) eq=true;
                else if(a->isInt()&&b->isInt()) eq=a->asInt()==b->asInt();
                else if((a->isInt()||a->isDbl())&&(b->isInt()||b->isDbl())) eq=a->asDbl()==b->asDbl();
                else if(a->isStr()&&b->isStr()) eq=a->asStr()==b->asStr();
                push(eq?TRUE_V:NIL_V); break;
            }
            case Op::NEG: { Val a=pop(); push(a->isInt()?mkInt(-a->asInt()):mkDbl(-a->asDbl())); break; }

            case Op::PRINT: {
                int n=(int)ins.arg; std::vector<Val> args(n);
                for(int i=n-1;i>=0;i--) args[i]=pop();
                for(int i=0;i<n;i++) { if(i) std::cout<<' '; std::cout<<args[i]->repr(); }
                std::cout<<'\n'; push(NIL_V); break;
            }
            case Op::SEQ: {
                int n=(int)ins.arg; Val last=pop();
                for(int i=0;i<n-1;i++) pop();
                push(last); break;
            }
            case Op::INPUT: {
                std::string line; std::getline(std::cin, line);
                push(mkStr(line)); break;
            }
            case Op::SQRT: { Val v=pop(); push(mkDbl(std::sqrt(v->asDbl()))); break; }

            // ── list ops ────────────────────────────────────────────────────
            case Op::LIST_LEN: { Val v=pop(); push(mkInt((int64_t)v->asList().size())); break; }
            case Op::LIST_SUM: {
                Val v=pop(); bool flt=false; double s=0;
                for(auto& x:v->asList()){if(x->isDbl())flt=true;s+=x->asDbl();}
                push(flt?mkDbl(s):mkInt((int64_t)s)); break;
            }
            case Op::LIST_SORT: {
                Val v=pop(); auto lst=v->asList();
                std::sort(lst.begin(),lst.end(),[](const Val&a,const Val&b){return a->asDbl()<b->asDbl();});
                push(mkList(std::move(lst))); break;
            }
            case Op::LIST_IDX: {
                Val idx=pop(),lst=pop();
                int64_t i=idx->isInt()?idx->asInt():(int64_t)idx->asDbl();
                push(lst->asList()[(size_t)i]); break;
            }
            case Op::LIST_FOLD: {
                Val f=pop(),init=pop(),xs=pop(); Val acc=init;
                for(auto& x:xs->asList()) acc=callClosure(f->asClosure(),{acc,x});
                push(acc); break;
            }
            case Op::LIST_MAP: {
                Val f=pop(),xs=pop(); std::vector<Val> res;
                for(auto& x:xs->asList()) res.push_back(callClosure(f->asClosure(),{x}));
                push(mkList(std::move(res))); break;
            }
            case Op::LIST_FILTER: {
                Val f=pop(),xs=pop(); std::vector<Val> res;
                for(auto& x:xs->asList()) if(callClosure(f->asClosure(),{x})->truthy()) res.push_back(x);
                push(mkList(std::move(res))); break;
            }
            case Op::LIST_ANY: {
                Val f=pop(),xs=pop(); bool found=false;
                for(auto& x:xs->asList()) if(callClosure(f->asClosure(),{x})->truthy()){found=true;break;}
                push(found?TRUE_V:NIL_V); break;
            }
            case Op::LIST_ALL: {
                Val f=pop(),xs=pop(); bool ok=true;
                for(auto& x:xs->asList()) if(!callClosure(f->asClosure(),{x})->truthy()){ok=false;break;}
                push(ok?TRUE_V:NIL_V); break;
            }
            case Op::LIST_FIND: {
                Val f=pop(),xs=pop(); Val found=NIL_V;
                for(auto& x:xs->asList()) if(callClosure(f->asClosure(),{x})->truthy()){found=x;break;}
                push(found); break;
            }
            case Op::LIST_FLAT: {
                Val v=pop(); std::vector<Val> res;
                for(auto& x:v->asList())
                    if(x->isList()) for(auto& y:x->asList()) res.push_back(y);
                    else res.push_back(x);
                push(mkList(std::move(res))); break;
            }
            case Op::RANGE: {
                int argc=(int)ins.arg;
                int64_t start=0,end_=0,step=1;
                if(argc==1){auto v=pop();end_=v->asInt();}
                else if(argc==2){auto e=pop();end_=e->asInt();auto s=pop();start=s->asInt();}
                else{auto st=pop();step=st->asInt();auto e=pop();end_=e->asInt();auto s=pop();start=s->asInt();}
                std::vector<Val> r;
                if(step>0) for(int64_t i=start;i<end_;i+=step) r.push_back(mkInt(i));
                else       for(int64_t i=start;i>end_;i+=step) r.push_back(mkInt(i));
                push(mkList(std::move(r))); break;
            }
            case Op::MATCH_NODE: {
                Val v=pop();
                if(!v->isNode()) throw std::runtime_error("match: not a node: "+v->repr());
                auto nd=v->asNode();
                push(nd->left); push(nd->right); push(nd->val); break;
            }

            // ── string ops ──────────────────────────────────────────────────
            case Op::STR_CAT: {
                int n=(int)ins.arg; std::vector<Val> args(n);
                for(int i=n-1;i>=0;i--) args[i]=pop();
                std::string s; for(auto& a:args) s+=a->repr();
                push(mkStr(s)); break;
            }
            case Op::STR_LEN:  { Val v=pop(); push(mkInt((int64_t)v->asStr().size())); break; }
            case Op::STR_GET: {
                Val idx=pop(),s=pop();
                int64_t i=idx->isInt()?idx->asInt():(int64_t)idx->asDbl();
                if(i<0||(size_t)i>=s->asStr().size()) push(NIL_V);
                else push(mkStr(std::string(1,s->asStr()[i]))); break;
            }
            case Op::STR_SUB: {
                Val e=pop(),a=pop(),s=pop();
                int64_t ai=a->isInt()?a->asInt():(int64_t)a->asDbl();
                int64_t ei=e->isInt()?e->asInt():(int64_t)e->asDbl();
                auto& str=s->asStr();
                if(ai<0) ai=0; int64_t sz2=(int64_t)str.size(); if(ei>sz2) ei=sz2;
                push(ei>ai?mkStr(str.substr(ai,ei-ai)):mkStr("")); break;
            }
            case Op::STR_SPLIT: {
                Val sep=pop(),sv=pop();
                std::string str=sv->asStr(),d=sep->asStr();
                std::vector<Val> res;
                if(d.empty()){for(char c:str) res.push_back(mkStr(std::string(1,c)));}
                else{size_t p=0,f;while((f=str.find(d,p))!=std::string::npos){res.push_back(mkStr(str.substr(p,f-p)));p=f+d.size();}res.push_back(mkStr(str.substr(p)));}
                push(mkList(std::move(res))); break;
            }
            case Op::STR_TRIM: {
                Val v=pop(); std::string s=v->asStr();
                size_t l=s.find_first_not_of(" \t\n\r"), r2=s.find_last_not_of(" \t\n\r");
                push(l==std::string::npos?mkStr(""):mkStr(s.substr(l,r2-l+1))); break;
            }
            case Op::STR_FIND: {
                Val sub=pop(),s=pop();
                size_t pos2=s->asStr().find(sub->asStr());
                push(pos2==std::string::npos?NIL_V:mkInt((int64_t)pos2)); break;
            }
            case Op::STR_NUM: {
                Val v=pop();
                try{size_t i;int64_t n=std::stoll(v->asStr(),&i);if(i==v->asStr().size()){push(mkInt(n));break;}}catch(...){}
                try{size_t i;double  n=std::stod (v->asStr(),&i);if(i==v->asStr().size()){push(mkDbl(n));break;}}catch(...){}
                push(NIL_V); break;
            }
            case Op::NUM_STR: { Val v=pop(); push(mkStr(v->repr())); break; }
            case Op::STR_UPPER: {
                Val v=pop(); std::string s=v->asStr();
                std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return (char)std::toupper(c);});
                push(mkStr(s)); break;
            }
            case Op::STR_LOWER: {
                Val v=pop(); std::string s=v->asStr();
                std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return (char)std::tolower(c);});
                push(mkStr(s)); break;
            }
            case Op::STR_STARTS: {
                Val pre=pop(),s=pop();
                auto& str=s->asStr(); auto& p=pre->asStr();
                push(str.size()>=p.size()&&str.substr(0,p.size())==p?TRUE_V:NIL_V); break;
            }
            case Op::STR_ENDS: {
                Val suf=pop(),s=pop();
                auto& str=s->asStr(); auto& sf=suf->asStr();
                push(str.size()>=sf.size()&&str.substr(str.size()-sf.size())==sf?TRUE_V:NIL_V); break;
            }
            case Op::STR_REPLACE: {
                Val to=pop(),fr=pop(),sv=pop();
                std::string str=sv->asStr(),from=fr->asStr(),t=to->asStr(),res;
                size_t p=0,f;
                while((f=str.find(from,p))!=std::string::npos){res+=str.substr(p,f-p)+t;p=f+from.size();}
                res+=str.substr(p); push(mkStr(res)); break;
            }

            // ── dict ops ────────────────────────────────────────────────────
            case Op::MAKE_DICT: push(mkDict()); break;
            case Op::DICT_SET: {
                Val v=pop(),k=pop(),d=pop();
                auto m=*d->asDict(); m[k->asStr()]=v;
                push(mkDict(std::move(m))); break;
            }
            case Op::DICT_GET: {
                Val k=pop(),d=pop();
                auto it=d->asDict()->find(k->asStr());
                push(it!=d->asDict()->end()?it->second:NIL_V); break;
            }
            case Op::DICT_HAS: {
                Val k=pop(),d=pop();
                push(d->asDict()->count(k->asStr())?TRUE_V:NIL_V); break;
            }
            case Op::DICT_DEL: {
                Val k=pop(),d=pop();
                auto m=*d->asDict(); m.erase(k->asStr());
                push(mkDict(std::move(m))); break;
            }
            case Op::DICT_KEYS: {
                Val d=pop(); std::vector<Val> ks;
                for(auto& [k,_]:*d->asDict()) ks.push_back(mkStr(k));
                push(mkList(std::move(ks))); break;
            }
            case Op::DICT_VALS: {
                Val d=pop(); std::vector<Val> vs;
                for(auto& [_,v]:*d->asDict()) vs.push_back(v);
                push(mkList(std::move(vs))); break;
            }
            case Op::DICT_LEN: { Val d=pop(); push(mkInt((int64_t)d->asDict()->size())); break; }
            case Op::DICT_MERGE: {
                Val b=pop(),a=pop();
                auto m=*a->asDict();
                for(auto& [k,v]:*b->asDict()) m[k]=v;
                push(mkDict(std::move(m))); break;
            }

            // ── misc ────────────────────────────────────────────────────────
            case Op::TYPE_OF: {
                Val v=pop();
                const char* t="unknown";
                if(v->isNil())     t="nil";
                else if(v->isBool())    t="bool";
                else if(v->isInt())     t="int";
                else if(v->isDbl())     t="float";
                else if(v->isStr())     t="str";
                else if(v->isList())    t="list";
                else if(v->isNode())    t="node";
                else if(v->isClosure()) t="fn";
                else if(v->isDict())    t="dict";
                push(mkStr(t)); break;
            }
            case Op::APPLY: {
                Val args=pop(),fn=pop();
                if(!fn->isClosure()) throw std::runtime_error("apply: not callable");
                push(callClosure(fn->asClosure(), args->asList())); break;
            }
            case Op::TRY: {
                // Stack: [thunk, handler]
                Val handler=pop(), thunk=pop();
                try {
                    push(callClosure(thunk->asClosure(), {}));
                } catch(const std::exception& ex) {
                    push(callClosure(handler->asClosure(), {mkStr(ex.what())}));
                }
                break;
            }
            case Op::EVAL: {
                Val v=pop();
                try {
                    Lexer  lx(v->asStr()); auto toks=lx.all();
                    Parser pr(toks);       auto prog=pr.program();
                    Compiler c2; c2.compProgram(prog, /*returnLast=*/true);
                    VM vm2(c2); vm2.globals=globals;
                    push(vm2.exec(c2.co, vm2.globals));
                } catch(const std::exception& ex) {
                    push(NIL_V);
                }
                break;
            }

            #undef BCAST_OP
            default: throw std::runtime_error("Unknown opcode "+std::to_string((int)ins.op));
            }
        }
        return stack.empty() ? NIL_V : stack.back();
    }

    void run() { exec(C.co, globals); }
};

// ══════════════════════════════════════════════════════════════════════════════
// MAIN
// ══════════════════════════════════════════════════════════════════════════════

static void runSource(const std::string& src) {
    Lexer  lx(src);   auto tokens =lx.all();
    Parser pr(tokens); auto program=pr.program();
    Compiler compiler; compiler.compProgram(program);
    VM vm(compiler);   vm.run();
}

#ifdef XN_BUNDLE
int main() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    try { runSource(XN_BUNDLE_SRC); }
    catch(const std::exception& e) { std::cerr<<"Error: "<<e.what()<<"\n"; return 1; }
    return 0;
}
#else

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    if(argc<2) {
        std::cerr<<"Usage:\n"
                   "  xnvm <file.xn>                    -- run\n"
                   "  xnvm --build <file.xn> [out.exe]  -- compile to standalone exe\n";
        return 1;
    }

    if(std::string(argv[1])=="--build") {
        if(argc<3) { std::cerr<<"--build: missing input file\n"; return 1; }
        std::string xnFile=argv[2];

        std::string outExe;
        if(argc>=4) { outExe=argv[3]; }
        else {
            outExe=xnFile;
            if(outExe.size()>3 && outExe.substr(outExe.size()-3)==".xn") outExe.resize(outExe.size()-3);
            outExe+=".exe";
        }

        std::ifstream xnf(xnFile);
        if(!xnf) { std::cerr<<"Cannot open: "<<xnFile<<"\n"; return 1; }
        std::string src((std::istreambuf_iterator<char>(xnf)),{});

        std::string xnvmCpp;
        {
#ifdef _WIN32
            wchar_t wbuf[4096]={};
            GetModuleFileNameW(nullptr, wbuf, 4095);
            int len=WideCharToMultiByte(CP_UTF8,0,wbuf,-1,nullptr,0,nullptr,nullptr);
            std::string exePath(len,'\0');
            WideCharToMultiByte(CP_UTF8,0,wbuf,-1,exePath.data(),len,nullptr,nullptr);
            exePath.resize(len>0?len-1:0);
#else
            std::string exePath=argv[0];
#endif
            auto slash=exePath.find_last_of("/\\");
            xnvmCpp=(slash!=std::string::npos)?exePath.substr(0,slash+1)+"xnvm.cpp":"xnvm.cpp";
        }

        std::string tmpCpp="_xn_bundle_tmp.cpp";
        {
            std::ofstream out(tmpCpp);
            if(!out) { std::cerr<<"Cannot write tmp file\n"; return 1; }
            out << "#define XN_BUNDLE\n";
            out << "static const char* XN_BUNDLE_SRC = R\"_XN_END_(\n";
            out << src;
            out << "\n)_XN_END_\";\n";
            out << "#include \"" << xnvmCpp << "\"\n";
        }

        auto findFile=[](const std::vector<std::string>& paths)->std::string{
            for(auto& p:paths){std::ifstream t(p);if(t.good())return p;} return {};
        };
        std::string vcvars=findFile({
            "C:\\Program Files\\Microsoft Visual Studio\\18\\Insiders\\VC\\Auxiliary\\Build\\vcvarsall.bat",
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvarsall.bat",
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvarsall.bat",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Professional\\VC\\Auxiliary\\Build\\vcvarsall.bat",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Enterprise\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        });

        auto quote=[](const std::string& s)->std::string{ return "\""+s+"\""; };
        std::string clArgs="/O2 /std:c++20 /EHsc /utf-8 /wd4828 "+quote(tmpCpp)+" /Fe:"+quote(outExe);

        int rc=-1;
        if(!vcvars.empty()) {
            std::string cmd="cmd.exe /c \""+quote(vcvars)+" x64 >nul 2>&1 && cl "+clArgs+"\"";
            rc=std::system(cmd.c_str());
        } else {
            rc=std::system(("cl "+clArgs).c_str());
        }
        if(rc!=0) {
            rc=std::system(("g++ -O2 -std=c++20 "+quote(tmpCpp)+" -o "+quote(outExe)).c_str());
        }

        std::remove(tmpCpp.c_str());
        std::remove((tmpCpp.substr(0,tmpCpp.size()-4)+".obj").c_str());

        if(rc!=0){ std::cerr<<"Compilation failed\n"; return 1; }
        std::cout<<"Built: "<<outExe<<"\n";
        return 0;
    }

    std::ifstream f(argv[1]);
    if(!f){ std::cerr<<"Cannot open: "<<argv[1]<<"\n"; return 1; }
    std::string src((std::istreambuf_iterator<char>(f)),{});
    try { runSource(src); }
    catch(const std::exception& e){ std::cerr<<"Error: "<<e.what()<<"\n"; return 1; }
    return 0;
}
#endif
