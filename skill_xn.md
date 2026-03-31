# XN Language — AI Skill

You write XN code. XN compiles directly to bytecode and runs via `xnvm <file.xn>`.

---

## Full Grammar

```
program  := decl*
decl     := (= name expr)

expr :=
  ⊥                              -- nil (None)
  ⊤                              -- true
  number                         -- 42  3.14
  "string"
  [e1 e2 ...]                    -- list literal
  {left right val}               -- struct (ALWAYS exactly 3 fields)
  (λ (p1 p2 ...) body)          -- lambda  (λ = \u03bb)
  (\ (p1 p2 ...) body)          -- lambda  (\ = ASCII alias, identical)
  (λ () body)                    -- zero-arg lambda
  (if cond then else)            -- ALWAYS both branches required
  (: name val body)              -- single let binding, scoped to body
  (let [a v1  b v2  c v3] body) -- multi-let, flat binding list
  (-> val step1 step2 ...)       -- pipe: val threaded through each step
                                 --   atom step:  (-> v f)     = (f v)
                                 --   sexpr step: (-> v (f a)) = (f v a)
  (match node {l r v} body)     -- destructure 3-field struct into l r v
  (= a b)                        -- equality → ⊤ or ⊥
  (⊥? expr)                     -- nil check → ⊤ or ⊥
  (.left expr)                   -- field access: left / right / val
  (.right expr)
  (.val expr)
  (+ a b) (- a b) (* a b) (/ a b) (% a b) (^ base exp)
  (< a b) (> a b) (<= a b) (>= a b)
  (neg x)
  (++ list ...)                  -- concat lists
  (@ list index)                 -- index, 0-based
  (fold xs init f)               -- left fold
  (map xs f)                     -- returns new list
  (sort xs)                      -- sorted copy
  (Σ xs)                         -- sum
  (# xs)                         -- length
  (√ x)                          -- sqrt
  (print arg ...)                -- print space-separated + newline
  (io expr ...)                  -- sequence side effects, return last
  (name arg ...)                 -- function call

-- BROADCASTING: arithmetic ops auto-broadcast over lists:
--   (+ [1 2 3] 10)      → [11 12 13]
--   (* [1 2 3] [4 5 6]) → [4 10 18]
```

---

## Rules

1. All operators are **prefix**: `(+ 1 2)` not `1 + 2`.
2. `(= name (λ ...))` → compiled as named `def`, supports recursion automatically.
3. Closures capture variables by reference — outer variables accessible inside inner `λ`.
4. `(: name val body)` binds `name` only inside `body`. Sequential lets are fine:
   ```xn
   (: a (compute x)
     (: b (use a)
       (result a b)))
   ```
5. Comparison ops return `⊤` (true) or `⊥` (nil/false) — both are truthy/falsy correctly.
6. `/` always returns float. Use `(% n 2)` for integer modulo.
7. `(@ list idx)` — index can be int or float (truncated to int automatically).

---

## Complete Example

```xn
-- Binary search tree + statistics

(= insert
  (λ (tree val)
    (if (⊥? tree)
      {⊥ ⊥ val}
      (if (< val (.val tree))
        {(insert (.left tree) val)  (.right tree)            (.val tree)}
        (if (> val (.val tree))
          {(.left tree)             (insert (.right tree) val) (.val tree)}
          tree)))))

(= search
  (λ (tree val)
    (if (⊥? tree)  ⊥
      (if (= val (.val tree))  ⊤
        (if (< val (.val tree))
          (search (.left  tree) val)
          (search (.right tree) val))))))

(= inorder
  (λ (tree)
    (if (⊥? tree)  []
      (++ (inorder (.left tree)) [(.val tree)] (inorder (.right tree))))))

(= build   (λ (xs)  (fold xs ⊥ insert)))
(= mean    (λ (xs)  (/ (Σ xs) (# xs))))
(= var     (λ (xs)  (/ (Σ (map xs (λ (x) (^ (- x (mean xs)) 2)))) (# xs))))
(= stddev  (λ (xs)  (√ (var xs))))
(= median
  (λ (xs)
    (: s (sort xs)  (: n (# s)
      (if (= (% n 2) 0)
        (/ (+ (@ s (- (/ n 2) 1)) (@ s (/ n 2))) 2.0)
        (@ s (/ n 2)))))))

(= main
  (λ ()
    (: data  [64 34 25 12 22 11 90 45 67 3]
      (: tree (build data)
        (: res  (inorder tree)
          (io
            (print "Sorted:    " res)
            (print "Mean:      " (mean   res))
            (print "Std dev:   " (stddev res))
            (print "Median:    " (median res))
            (print "Search 45: " (search tree 45))
            (print "Search 99: " (search tree 99))))))))
```

---

## Common Patterns

**Recursive with base case:**
```xn
(= fact (λ (n) (if (= n 0) 1 (* n (fact (- n 1))))))
```

**Filter via fold:**
```xn
(= keep-pos
  (λ (xs)
    (fold xs [] (λ (acc x) (if (> x 0) (++ acc [x]) acc)))))
```

**Map + reduce pipeline:**
```xn
(= sum-squares
  (λ (xs) (Σ (map xs (λ (x) (* x x))))))
```

**Mutually recursive (declare both before use — XN resolves via shared env):**
```xn
(= even? (λ (n) (if (= n 0) ⊤ (odd?  (- n 1)))))
(= odd?  (λ (n) (if (= n 0) ⊥ (even? (- n 1)))))
```

---

## New Features (v2)

**Pipe `->` — flatten deep nesting:**
```xn
-- old:
(print (√ (/ (Σ (map xs (\ (x) (* x x)))) (# xs))))
-- new:
(-> xs (map (\ (x) (* x x))) Σ (/ (# xs)) √ print)
```

**Multi-let — replace cascading `:`:**
```xn
-- old:
(: a 10 (: b 20 (: c 30 (+ a (+ b c)))))
-- new:
(let [a 10  b 20  c 30] (+ a (+ b c)))
```

**Match — destructure struct in one step:**
```xn
-- old:
(: l (.left nd) (: r (.right nd) (: v (.val nd) body)))
-- new:
(match nd {l r v} body)
```

**Broadcasting — scalar/list arithmetic:**
```xn
(+ [1 2 3] 10)        -- [11 12 13]
(* [2 3 4] [10 10 10]) -- [20 30 40]
(/ [10 20 30] 10)     -- [1.0 2.0 3.0]
```

**Short lambda `\` — ASCII alias for λ:**
```xn
(\ (x y) (+ x y))   -- same as (λ (x y) (+ x y))
```

---

## Errors to Avoid

| Wrong | Right |
|-------|-------|
| `(if cond then)` | `(if cond then ⊥)` |
| `{a b}` — 2 fields | `{a b c}` — always 3 |
| `(λ x body)` | `(λ (x) body)` |
| `a.field` | `(.field a)` |
| `[1, 2, 3]` | `[1 2 3]` |
| `return x` | last expr is return value |
| `x = 5` in body | `(: x 5 body)` or `(let [x 5] body)` |
| `(let [a 10] body)` — odd binding list | pairs only: `[a 10  b 20]` |

---

## Running

```sh
xnvm program.xn
```

Compile xnvm from source (requires C++20):
```sh
# GCC/Clang
g++ -O2 -std=c++20 xnvm.cpp -o xnvm

# MSVC
cl /O2 /std:c++20 /EHsc /utf-8 xnvm.cpp /Fe:xnvm.exe
```
