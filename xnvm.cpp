// xnvm.cpp -- XN bytecode compiler + stack VM
// Compile (MSVC): cl /O2 /std:c++20 /EHsc /utf-8 xnvm.cpp /Fe:xnvm.exe
// Compile (GCC) : g++ -O2 -std=c++20 xnvm.cpp -o xnvm
// Usage: xnvm <file.xn>

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

// ══════════════════════════════════════════════════════════════════════════════
// BYTECODE
// ══════════════════════════════════════════════════════════════════════════════

enum class Op : uint8_t {
    PUSH_CONST,     // CONST_POOL[arg]
    PUSH_NIL,
    PUSH_TRUE,
    LOAD,           // env.get(names[arg])
    STORE,          // env.set(names[arg], pop())
    MAKE_CLOSURE,   // pool[arg] + env -> closure
    CALL,           // pop arg vals + fn -> call
    RETURN,
    JUMP,
    JUMP_IF_FALSE,  // pop; if !truthy -> jump arg
    POP,
    IS_NIL,
    GET_FIELD,      // 0=left 1=right 2=val
    MAKE_NODE,      // pop left,right,val -> node
    MAKE_LIST,      // pop arg items -> list
    CONCAT,         // pop arg lists -> merged list
    ADD, SUB, MUL, DIV, MOD, POW,
    LT, GT, EQ, LE, GE,
    NEG,
    PRINT,          // pop arg values, print
    LIST_LEN,
    LIST_SUM,
    LIST_SORT,
    LIST_IDX,       // pop idx,list -> list[idx]
    LIST_FOLD,      // pop f,init,xs -> fold
    LIST_MAP,       // pop f,xs -> map
    SQRT,
    SEQ,            // pop arg items, keep last (io)
    MATCH_NODE,     // pop node -> push left,right,val onto stack
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

struct XNode { Val left, right, val; };

struct Closure { Code code; EnvP env; };

struct CodeObj {
    std::string              name;
    int                      arity = 0;
    std::vector<Instr>       code;
    std::vector<std::string> names;
    std::vector<Val>         consts;

    int nameIdx(const std::string& s) {
        for (int i = 0; i < (int)names.size(); i++)
            if (names[i] == s) return i;
        names.push_back(s);
        return (int)names.size() - 1;
    }
    int addConst(Val v) {
        consts.push_back(v);
        return (int)consts.size() - 1;
    }
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
        std::shared_ptr<Closure>  // CLOSURE
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

    int64_t     asInt()  const { return std::get<int64_t>(data); }
    bool        asBool() const { return std::get<bool>(data); }
    double      asDbl()  const {
        if (isInt()) return (double)std::get<int64_t>(data);
        return std::get<double>(data);
    }
    const std::string&        asStr()     const { return std::get<std::string>(data); }
    const std::vector<Val>&   asList()    const { return std::get<std::vector<Val>>(data); }
    std::vector<Val>&         asList()          { return std::get<std::vector<Val>>(data); }
    std::shared_ptr<XNode>    asNode()    const { return std::get<std::shared_ptr<XNode>>(data); }
    std::shared_ptr<Closure>  asClosure() const { return std::get<std::shared_ptr<Closure>>(data); }

    bool truthy() const {
        if (isNil())  return false;
        if (isBool()) return asBool();
        return true;
    }

    std::string repr() const {
        if (isNil())  return "None";
        if (isBool()) return asBool() ? "True" : "False";
        if (isInt())  return std::to_string(asInt());
        if (isDbl()) {
            std::ostringstream os; os << asDbl(); return os.str();
        }
        if (isStr())  return asStr();
        if (isList()) {
            std::string s = "[";
            for (int i = 0; i < (int)asList().size(); i++) {
                if (i) s += ", ";
                s += asList()[i]->repr();
            }
            return s + "]";
        }
        if (isNode())    return "Node(" + asNode()->val->repr() + ")";
        if (isClosure()) return "<closure:" + asClosure()->code->name + ">";
        return "?";
    }
};

static Val NIL_V  = std::make_shared<Value>(Value{std::monostate{}});
static Val TRUE_V = std::make_shared<Value>(Value{true});

inline Val mkInt (int64_t v)       { return std::make_shared<Value>(Value{v}); }
inline Val mkDbl (double  v)       { return std::make_shared<Value>(Value{v}); }
inline Val mkStr (std::string v)   { return std::make_shared<Value>(Value{std::move(v)}); }
inline Val mkList(std::vector<Val> v) { return std::make_shared<Value>(Value{std::move(v)}); }
inline Val mkNode(Val l, Val r, Val v) {
    return std::make_shared<Value>(Value{std::make_shared<XNode>(XNode{l,r,v})});
}
inline Val mkClosure(Code c, EnvP e) {
    return std::make_shared<Value>(Value{std::make_shared<Closure>(Closure{c,e})});
}

// ══════════════════════════════════════════════════════════════════════════════
// ENVIRONMENT  (linked frames -- enables closures & recursion)
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
            if (src[pos]=='-' && pos+1<src.size() && src[pos+1]=='-') {
                while (pos<src.size() && src[pos]!='\n') pos++;
            } else if ((unsigned char)src[pos]<=32) {
                pos++;
            } else break;
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
            while (pos<src.size()&&src[pos]!='"') {
                if (src[pos]=='\\') pos++;
                s+=src[pos++];
            }
            pos++;
            return {TK::STR, s};
        }
        // atom or number: advance byte-by-byte, respect UTF-8 multibyte
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
    std::string    sval;
    int64_t        ival = 0;
    double         fval = 0.0;
    std::vector<AST> ch;
};

static AST makeANode(ANode::K k) { auto n=std::make_shared<ANode>(); n->kind=k; return n; }

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
        default:
            throw std::runtime_error("Unexpected token: '" + t.val + "'");
        }
    }

    std::vector<AST> program() {
        std::vector<AST> r;
        while(peek().type!=TK::END) r.push_back(expr());
        return r;
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// COMPILER  (AST -> bytecode)
// ══════════════════════════════════════════════════════════════════════════════

struct Compiler {
    std::vector<Code> pool;  // all compiled function code objects
    Code              co;    // currently-being-compiled code object

    Compiler() { co=std::make_shared<CodeObj>(); co->name="<module>"; }

    void     emit(Op op, int64_t arg=0) { co->code.push_back({op,arg}); }
    int      addConst(Val v)            { return co->addConst(v); }
    int      ip()                       { return (int)co->code.size(); }
    void     patch(int idx)             { co->code[idx].arg=ip(); }

    static std::string atomOf(AST n) {
        return (n->kind==ANode::K::ATOM) ? n->sval : "";
    }

    // ── expression ────────────────────────────────────────────────────────────
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

    // ── s-expression dispatch ─────────────────────────────────────────────────
    void compSexpr(AST n) {
        auto& ch=n->ch;
        if(ch.empty()) { emit(Op::PUSH_NIL); return; }
        std::string h=atomOf(ch[0]);

        if(h=="\xce\xbb"||h=="\\") { // λ or \ (short alias)
            compLambda(ch[1], ch[2]);
            return;
        }
        // (-> val step1 step2 ...)
        // Pure AST rewrite — no extra opcodes needed.
        //   atom  step: (-> v f)       =>  (f v)
        //   sexpr step: (-> v (f a b)) =>  (f v a b)   value threaded as first arg
        if(h=="->") {
            AST cur=ch[1];
            for(int i=2;i<(int)ch.size();i++) {
                AST step=ch[i];
                auto call=makeANode(ANode::K::SEXPR);
                if(step->kind==ANode::K::ATOM) {
                    call->ch.push_back(step);
                    call->ch.push_back(cur);
                } else {
                    call->ch.push_back(step->ch[0]);      // fn
                    call->ch.push_back(cur);               // threaded value first
                    for(int j=1;j<(int)step->ch.size();j++)
                        call->ch.push_back(step->ch[j]);  // extra args
                }
                cur=call;
            }
            compExpr(cur);
            return;
        }
        // (let [a v1  b v2  c v3] body)  — multi-let, flat binding list
        if(h=="let") {
            auto& binds=ch[1]->ch;
            for(int i=0;i+1<(int)binds.size();i+=2) {
                compExpr(binds[i+1]);
                emit(Op::STORE, co->nameIdx(binds[i]->sval));
            }
            compExpr(ch[2]);
            return;
        }
        // (match node {l r v} body)  — struct destructuring
        if(h=="match") {
            compExpr(ch[1]);       // push node
            emit(Op::MATCH_NODE);  // pop node -> push left right val
            auto& pat=ch[2]->ch;   // STRUCT {l r v}
            emit(Op::STORE, co->nameIdx(pat[2]->sval)); // val   (top of stack)
            emit(Op::STORE, co->nameIdx(pat[1]->sval)); // right
            emit(Op::STORE, co->nameIdx(pat[0]->sval)); // left
            compExpr(ch[3]);
            return;
        }
        if(h=="if") {
            compExpr(ch[1]);
            int jf=ip(); emit(Op::JUMP_IF_FALSE,0);
            compExpr(ch[2]);
            int je=ip(); emit(Op::JUMP,0);
            patch(jf);
            compExpr(ch[3]);
            patch(je);
            return;
        }
        if(h==":") { // let: (: name val body)
            compExpr(ch[2]);
            emit(Op::STORE, co->nameIdx(ch[1]->sval));
            compExpr(ch[3]);
            return;
        }
        if(h=="\xe2\x8a\xa5?") { // ⊥?
            compExpr(ch[1]); emit(Op::IS_NIL); return;
        }
        // (.field expr)
        if(!h.empty()&&h[0]=='.') {
            std::string f=h.substr(1);
            int fi=(f=="left")?0:(f=="right")?1:2;
            compExpr(ch[1]); emit(Op::GET_FIELD,fi); return;
        }
        if(h=="++") {
            for(int i=1;i<(int)ch.size();i++) compExpr(ch[i]);
            emit(Op::CONCAT,(int)ch.size()-1); return;
        }
        if(h=="io") {
            for(int i=1;i<(int)ch.size();i++) compExpr(ch[i]);
            emit(Op::SEQ,(int)ch.size()-1); return;
        }
        if(h=="fold") { compExpr(ch[1]);compExpr(ch[2]);compExpr(ch[3]); emit(Op::LIST_FOLD); return; }
        if(h=="map")  { compExpr(ch[1]);compExpr(ch[2]); emit(Op::LIST_MAP);  return; }
        if(h=="sort") { compExpr(ch[1]); emit(Op::LIST_SORT); return; }
        if(h=="@")    { compExpr(ch[1]);compExpr(ch[2]); emit(Op::LIST_IDX);  return; }
        if(h=="print"){ for(int i=1;i<(int)ch.size();i++) compExpr(ch[i]); emit(Op::PRINT,(int)ch.size()-1); return; }
        if(h=="\xce\xa3") { compExpr(ch[1]); emit(Op::LIST_SUM); return; } // Σ
        if(h=="#")    { compExpr(ch[1]); emit(Op::LIST_LEN); return; }
        if(h=="\xe2\x88\x9a") { compExpr(ch[1]); emit(Op::SQRT); return; } // √

        // binary arithmetic / comparison
        static const std::pair<const char*,Op> BINOPS[] = {
            {"+",Op::ADD},{"-",Op::SUB},{"*",Op::MUL},{"/",Op::DIV},
            {"%",Op::MOD},{"^",Op::POW},
            {"<",Op::LT},{">",Op::GT},{"=",Op::EQ},{"<=",Op::LE},{">=",Op::GE}
        };
        for(auto& [sym,op]:BINOPS)
            if(h==sym) { compExpr(ch[1]); compExpr(ch[2]); emit(op); return; }

        if(h=="neg") { compExpr(ch[1]); emit(Op::NEG); return; }

        // general function call
        compExpr(ch[0]);
        for(int i=1;i<(int)ch.size();i++) compExpr(ch[i]);
        emit(Op::CALL,(int)ch.size()-1);
    }

    // ── lambda compilation ────────────────────────────────────────────────────
    void compLambda(AST params, AST body, const std::string& name="<lambda>") {
        auto saved=co;
        co=std::make_shared<CodeObj>();
        co->name=name;
        if(params->kind==ANode::K::SEXPR)
            for(auto& p:params->ch) { co->nameIdx(p->sval); co->arity++; }
        compExpr(body);
        emit(Op::RETURN);
        int idx=(int)pool.size(); pool.push_back(co);
        co=saved;
        emit(Op::MAKE_CLOSURE,idx);
    }

    // ── top-level declaration ─────────────────────────────────────────────────
    void compDecl(AST n) {
        // n = (= name expr)
        auto& ch=n->ch;
        std::string name=ch[1]->sval;
        AST expr=ch[2];

        bool isLam=(expr->kind==ANode::K::SEXPR
                    && !expr->ch.empty()
                    && expr->ch[0]->sval=="\xce\xbb");
        if(isLam)
            compLambda(expr->ch[1], expr->ch[2], name);
        else
            compExpr(expr);

        emit(Op::STORE, co->nameIdx(name));
        // STORE already consumed the value — no POP needed
    }

    // ── program ───────────────────────────────────────────────────────────────
    void compProgram(const std::vector<AST>& prog) {
        bool hasMain=false;
        for(auto& n:prog) {
            bool isDecl=(n->kind==ANode::K::SEXPR
                         && !n->ch.empty()
                         && n->ch[0]->sval=="=");
            if(isDecl) {
                if(n->ch[1]->sval=="main") hasMain=true;
                compDecl(n);  // STORE consumes value, stack stays clean
            } else {
                compExpr(n); emit(Op::POP);
            }
        }
        if(hasMain) { emit(Op::LOAD,co->nameIdx("main")); emit(Op::CALL,0); emit(Op::POP); }
        emit(Op::RETURN);
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// VIRTUAL MACHINE
// ══════════════════════════════════════════════════════════════════════════════

struct VM {
    Compiler& C;
    EnvP      globals;

    explicit VM(Compiler& c) : C(c), globals(std::make_shared<Env>()) {}

    Val callClosure(const std::shared_ptr<Closure>& clo, std::vector<Val> args) {
        auto& co=clo->code;
        if((int)args.size()!=co->arity)
            throw std::runtime_error(co->name+": expected "+std::to_string(co->arity)
                +" args, got "+std::to_string(args.size()));
        auto frame=std::make_shared<Env>(clo->env);
        for(int i=0;i<co->arity;i++) frame->set(co->names[i], args[i]);
        return exec(co, frame);
    }

    Val exec(Code co, EnvP env) {
        std::vector<Val> stack;
        stack.reserve(64);
        int ip=0;

        auto push = [&](Val v)  { stack.push_back(std::move(v)); };
        auto pop  = [&]() -> Val { Val v=stack.back(); stack.pop_back(); return v; };

        while(ip<(int)co->code.size()) {
            auto& ins=co->code[ip++];
            switch(ins.op) {

            case Op::PUSH_CONST:   push(co->consts[ins.arg]); break;
            case Op::PUSH_NIL:     push(NIL_V);  break;
            case Op::PUSH_TRUE:    push(TRUE_V); break;
            case Op::LOAD:         push(env->get(co->names[ins.arg])); break;
            case Op::STORE:        env->set(co->names[ins.arg], pop()); break;

            case Op::MAKE_CLOSURE:
                // Closure captures current env by shared_ptr.
                // Since globals is mutated by STORE *after* MAKE_CLOSURE,
                // and closure holds the same shared_ptr -> recursion works.
                push(mkClosure(C.pool[ins.arg], env));
                break;

            case Op::CALL: {
                int n=(int)ins.arg;
                std::vector<Val> args(n);
                for(int i=n-1;i>=0;i--) args[i]=pop();
                Val fn=pop();
                if(!fn->isClosure()) throw std::runtime_error("Not callable: "+fn->repr());
                push(callClosure(fn->asClosure(), std::move(args)));
                break;
            }
            case Op::RETURN:
                return stack.empty() ? NIL_V : pop();

            case Op::JUMP:          ip=(int)ins.arg; break;
            case Op::JUMP_IF_FALSE: { Val c=pop(); if(!c->truthy()) ip=(int)ins.arg; break; }
            case Op::POP:           pop(); break;

            case Op::IS_NIL: { Val v=pop(); push(v->isNil()?TRUE_V:NIL_V); break; }

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

            // BCAST_OP: if either operand is a list, broadcast op element-wise.
            // OP uses local names 'a' and 'b' (the two operands).
            // Static lambda shadows outer a_/b_ with its own a/b params -> no capture needed.
            #define BCAST_OP(OP) \
                { Val b_=pop(), a_=pop(); \
                  auto F=[](Val a, Val b) -> Val { return OP; }; \
                  if(!a_->isList()&&!b_->isList()) { push(F(a_,b_)); break; } \
                  if(a_->isList()&&b_->isList()) { \
                      auto& la=a_->asList(); auto& lb=b_->asList(); \
                      std::vector<Val> r; r.reserve(la.size()); \
                      for(int _=0;_<(int)la.size();_++) r.push_back(F(la[_],lb[_])); \
                      push(mkList(std::move(r))); \
                  } else if(a_->isList()) { \
                      std::vector<Val> r; \
                      for(auto& x:a_->asList()) r.push_back(F(x,b_)); \
                      push(mkList(std::move(r))); \
                  } else { \
                      std::vector<Val> r; \
                      for(auto& y:b_->asList()) r.push_back(F(a_,y)); \
                      push(mkList(std::move(r))); \
                  } break; }

            case Op::ADD: BCAST_OP(a->isInt()&&b->isInt()?mkInt(a->asInt()+b->asInt()):mkDbl(a->asDbl()+b->asDbl()))
            case Op::SUB: BCAST_OP(a->isInt()&&b->isInt()?mkInt(a->asInt()-b->asInt()):mkDbl(a->asDbl()-b->asDbl()))
            case Op::MUL: BCAST_OP(a->isInt()&&b->isInt()?mkInt(a->asInt()*b->asInt()):mkDbl(a->asDbl()*b->asDbl()))
            case Op::DIV: BCAST_OP(mkDbl(a->asDbl()/b->asDbl()))
            case Op::MOD: BCAST_OP(mkInt(a->asInt()%b->asInt()))
            case Op::POW: BCAST_OP(mkDbl(std::pow(a->asDbl(),b->asDbl())))

            case Op::LT:  { Val b=pop(),a=pop(); push(a->asDbl()<b->asDbl()  ? TRUE_V:NIL_V); break; }
            case Op::GT:  { Val b=pop(),a=pop(); push(a->asDbl()>b->asDbl()  ? TRUE_V:NIL_V); break; }
            case Op::LE:  { Val b=pop(),a=pop(); push(a->asDbl()<=b->asDbl() ? TRUE_V:NIL_V); break; }
            case Op::GE:  { Val b=pop(),a=pop(); push(a->asDbl()>=b->asDbl() ? TRUE_V:NIL_V); break; }
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
            case Op::LIST_LEN:  { Val v=pop(); push(mkInt((int64_t)v->asList().size())); break; }
            case Op::LIST_SUM: {
                Val v=pop(); bool flt=false; double s=0;
                for(auto& x:v->asList()) { if(x->isDbl()) flt=true; s+=x->asDbl(); }
                push(flt?mkDbl(s):mkInt((int64_t)s)); break;
            }
            case Op::LIST_SORT: {
                Val v=pop(); auto lst=v->asList();
                std::sort(lst.begin(),lst.end(),[](const Val& a,const Val& b){ return a->asDbl()<b->asDbl(); });
                push(mkList(std::move(lst))); break;
            }
            case Op::LIST_IDX: {
                Val idx=pop(), lst=pop();
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
            case Op::SQRT: { Val v=pop(); push(mkDbl(std::sqrt(v->asDbl()))); break; }
            case Op::SEQ: {
                int n=(int)ins.arg; Val last=pop();
                for(int i=0;i<n-1;i++) pop();
                push(last); break;
            }
            case Op::MATCH_NODE: {
                Val v=pop();
                if(!v->isNode()) throw std::runtime_error("match: not a node: "+v->repr());
                auto nd=v->asNode();
                push(nd->left); push(nd->right); push(nd->val); // left bottom, val top
                break;
            }
            #undef BCAST_OP
            default:
                throw std::runtime_error("Unknown opcode "+std::to_string((int)ins.op));
            }
        }
        return stack.empty() ? NIL_V : stack.back();
    }

    void run() { exec(C.co, globals); }
};

// ══════════════════════════════════════════════════════════════════════════════
// MAIN
// ══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    if(argc<2) { std::cerr<<"Usage: xnvm <file.xn>\n"; return 1; }
    std::ifstream f(argv[1]);
    if(!f) { std::cerr<<"Cannot open: "<<argv[1]<<"\n"; return 1; }
    std::string src((std::istreambuf_iterator<char>(f)),{});
    try {
        Lexer  lx(src);   auto tokens =lx.all();
        Parser pr(tokens); auto program=pr.program();
        Compiler compiler; compiler.compProgram(program);
        VM vm(compiler);   vm.run();
    } catch(const std::exception& e) {
        std::cerr<<"Error: "<<e.what()<<"\n"; return 1;
    }
    return 0;
}
